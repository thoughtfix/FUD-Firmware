// Cardputer ADV + Hydra RF bring-up test (env cardputer-hwtest). Diagnostic only, not FUD.
// Built alone: the env's build_src_filter leaves main.cpp out. Empty in every other env.
//
// Proves each part works, and that the display and the Hydra/SD bus run at the same time:
//   core 0 task owns the shared FSPI bus (CC1101, nRF24, SD) plus passive Wi-Fi and BLE.
//   core 1 loop redraws the screen nonstop (display is on M5GFX's SPI3), reads GPS, keys, IMU.
//
// Receive only. CC1101: SRES, SCAL, SRX, SIDLE and register writes, never STX. nRF24: PRIM_RX
// only, never a payload. Wi-Fi and BLE scans are passive. The one transmitter is IR, and only
// when you type `ir` on serial. Serial: help, report, beep, ir, gpsraw.
#ifdef FUD_HWTEST_CARDPUTER
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

// Cardputer ADV + Hydra RF pins. Radios and SD share one bus (FSPI); the display has SPI3.
static const int P_SCK = 40, P_MISO = 39, P_MOSI = 14, P_SD_CS = 12;
static const int P_CC_CS = 13, P_CC_GDO0 = 5, P_NRF_CSN = 6, P_NRF_CE = 4;
static const int P_IR = 44;                 // IR LED (G46 is the codec's mic line on the ADV)
static const int P_GROVE_A = 1, P_GROVE_B = 2;
static const uint8_t TCA = 0x34;            // TCA8418 keyboard, internal I2C 8/9

SPIClass bus(FSPI);
IRsend* ir = nullptr;
static const SPISettings RF_SPI(1000000, MSBFIRST, SPI_MODE0);

// Results. Written by the bus task or the loop, read by the screen (display-only race is fine).
struct Res {
  bool isAdv = false; uint32_t psram = 0;
  uint8_t ccVer = 0, ccPart = 0xFF; bool ccOk = false;
  int pk433 = -200, fl433 = 200, pk915 = -200, fl915 = 200;
  bool nrfOk = false; int nrfHot = -1;
  bool sdOk = false; uint8_t sdType = 0; uint32_t sdMB = 0; uint32_t sdPass = 0, sdFail = 0;
  int wifiN = -1, bleN = -1;
  int gpsPin = -1; uint32_t gpsBaud = 0, gpsBytes = 0, gpsGood = 0, gpsBad = 0;
  char gpsFix = '-'; int gpsSats = -1; char gpsLat[16] = "", gpsLon[16] = "", gpsUtc[12] = "";
  float ax = 0, ay = 0, az = 0; bool imuOk = false;
  int batMv = -1, batPct = -1, chg = -1;
  bool keyOk = false; int keyCode = -1; bool keyDown = false; uint32_t keyN = 0;
  uint32_t cycles = 0, frames = 0;
} R;

// ---------------- CC1101 (receive only) ----------------
static void ccSel() { digitalWrite(P_CC_CS, LOW); uint32_t t = micros(); while (digitalRead(P_MISO) && micros() - t < 5000) {} }
static void ccDesel() { digitalWrite(P_CC_CS, HIGH); }
static void ccStrobe(uint8_t c) { ccSel(); bus.transfer(c); ccDesel(); }
static void ccWrite(uint8_t a, uint8_t v) { ccSel(); bus.transfer(a); bus.transfer(v); ccDesel(); }
static uint8_t ccStatus(uint8_t a) { ccSel(); bus.transfer(0xC0 | a); uint8_t v = bus.transfer(0); ccDesel(); return v; }
static int ccRssi() { uint8_t r = ccStatus(0x34); int d = (r >= 128) ? (r - 256) : r; return d / 2 - 74; }

static void ccProbe() {
  bus.beginTransaction(RF_SPI);
  ccDesel(); delayMicroseconds(30); digitalWrite(P_CC_CS, LOW); delayMicroseconds(30); ccDesel(); delayMicroseconds(45);
  ccStrobe(0x30); delay(1);                               // SRES
  R.ccPart = ccStatus(0x30); R.ccVer = ccStatus(0x31);    // PARTNUM, VERSION
  bus.endTransaction();
  R.ccOk = R.ccVer != 0x00 && R.ccVer != 0xFF;
}
// Carrier sense at one frequency: widest RX bandwidth, OOK, no sync. ~1 s of RSSI samples.
static void ccSample(uint32_t hz, int& pk, int& fl) {
  bus.beginTransaction(RF_SPI);
  ccStrobe(0x30); delay(1);
  uint32_t w = (uint32_t)(((uint64_t)hz << 16) / 26000000ULL);
  ccWrite(0x0D, (w >> 16) & 0xFF); ccWrite(0x0E, (w >> 8) & 0xFF); ccWrite(0x0F, w & 0xFF);
  ccWrite(0x10, 0x07); ccWrite(0x12, 0x30); ccWrite(0x0B, 0x06);
  ccStrobe(0x33); delay(1); ccStrobe(0x34); delay(2);    // SCAL, SRX
  int p = -200, f = 200;
  for (int i = 0; i < 60; i++) { int r = ccRssi(); if (r > p) p = r; if (r < f) f = r; delay(15); }
  ccStrobe(0x36);                                         // SIDLE
  bus.endTransaction();
  pk = p; fl = f;
}

// ---------------- nRF24 (receive only) ----------------
static uint8_t nrfRead(uint8_t r) { digitalWrite(P_NRF_CSN, LOW); bus.transfer(r & 0x1F); uint8_t v = bus.transfer(0xFF); digitalWrite(P_NRF_CSN, HIGH); return v; }
static void nrfWrite(uint8_t r, uint8_t v) { digitalWrite(P_NRF_CSN, LOW); bus.transfer(0x20 | (r & 0x1F)); bus.transfer(v); digitalWrite(P_NRF_CSN, HIGH); }
static void nrfProbe() {
  digitalWrite(P_NRF_CE, LOW);
  bus.beginTransaction(RF_SPI); nrfWrite(0x05, 0x4C); uint8_t rb = nrfRead(0x05); bus.endTransaction();
  R.nrfOk = (rb == 0x4C);
}
// Carrier detect (RPD) on all 126 channels. Counts the busy ones.
static void nrfSweep() {
  bus.beginTransaction(RF_SPI);
  nrfWrite(0x00, 0x0B); delay(2);                         // PWR_UP + PRIM_RX
  int hot = 0;
  for (int ch = 0; ch <= 125; ch++) {
    nrfWrite(0x05, ch); digitalWrite(P_NRF_CE, HIGH); delayMicroseconds(200); digitalWrite(P_NRF_CE, LOW);
    if (nrfRead(0x09) & 0x01) hot++;
  }
  nrfWrite(0x00, 0x09);                                   // power down, still PRIM_RX
  bus.endTransaction();
  R.nrfHot = hot;
}

// ---------------- SD (same bus) ----------------
static void sdMount() {
  R.sdOk = SD.begin(P_SD_CS, bus, 20000000);
  if (!R.sdOk) { SD.end(); R.sdOk = SD.begin(P_SD_CS, bus, 4000000); }
  if (R.sdOk) { R.sdType = SD.cardType(); R.sdMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL)); }
}
// Write one line to /fud-hwtest.txt, read it back, compare. Between radio phases, so it proves
// the card still works after the radios used the bus.
static void sdRoundTrip() {
  if (!R.sdOk) { sdMount(); if (!R.sdOk) { R.sdFail++; return; } }
  char want[64]; snprintf(want, sizeof(want), "fud hwtest cycle %lu ms %lu", (unsigned long)R.cycles, (unsigned long)millis());
  File f = SD.open("/fud-hwtest.txt", FILE_WRITE);
  if (!f) { R.sdFail++; R.sdOk = false; SD.end(); return; }
  f.println(want); f.close();
  f = SD.open("/fud-hwtest.txt", FILE_READ);
  if (!f) { R.sdFail++; return; }
  String got = f.readStringUntil('\n'); f.close(); got.trim();
  if (got == want) R.sdPass++; else R.sdFail++;
}

// ---------------- bus task (core 0) ----------------
static void busTask(void*) {
  NimBLEScan* s = NimBLEDevice::getScan(); s->setActiveScan(false); s->setInterval(160); s->setWindow(80);
  for (;;) {
    if (R.ccOk) { ccSample(433920000UL, R.pk433, R.fl433); ccSample(915000000UL, R.pk915, R.fl915); }
    sdRoundTrip();
    if (R.nrfOk) nrfSweep();
    sdRoundTrip();
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    R.wifiN = WiFi.scanNetworks(false, true, true, 120);   // passive: beacons only
    WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
    NimBLEScanResults r = s->getResults(3000, false);       // passive: advertisements only
    R.bleN = r.getCount(); s->clearResults();
    R.cycles++;
    Serial.printf("[cycle %lu] cc433 pk %d fl %d | cc915 pk %d fl %d | nrf hot %d | sd %lu/%lu | wifi %d | ble %d | heap %u min %u\n",
                  (unsigned long)R.cycles, R.pk433, R.fl433, R.pk915, R.fl915, R.nrfHot,
                  (unsigned long)R.sdPass, (unsigned long)(R.sdPass + R.sdFail), R.wifiN, R.bleN,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------------- GPS (Grove, listen only) ----------------
static char gLine[100]; static int gLen = 0; static bool gpsRaw = false;
static bool nmeaOk(const char* s) {
  if (s[0] != '$') return false; const char* star = strchr(s, '*'); if (!star || strlen(star) < 3) return false;
  uint8_t c = 0; for (const char* p = s + 1; p < star; p++) c ^= (uint8_t)*p;
  return c == (uint8_t)strtol(star + 1, nullptr, 16);
}
static int fields(char* s, char* out[], int maxN) {      // split on commas, in place
  int n = 0; out[n++] = s; for (char* p = s; *p && n < maxN; p++) if (*p == ',' || *p == '*') { *p = 0; out[n++] = p + 1; }
  return n;
}
static void nmeaLine(char* s) {
  if (gpsRaw) Serial.printf("[gps] %s\n", s);
  if (!nmeaOk(s)) { R.gpsBad++; return; }
  R.gpsGood++;
  char* f[20]; int n = fields(s, f, 20);
  const char* t = f[0] + 3;                               // skip $GP / $GN
  if (!strcmp(t, "RMC") && n > 6) {
    R.gpsFix = f[2][0] ? f[2][0] : '-';
    snprintf(R.gpsUtc, sizeof(R.gpsUtc), "%.6s", f[1]);
    if (f[3][0]) snprintf(R.gpsLat, sizeof(R.gpsLat), "%s%s", f[3], f[4]); else R.gpsLat[0] = 0;
    if (f[5][0]) snprintf(R.gpsLon, sizeof(R.gpsLon), "%s%s", f[5], f[6]); else R.gpsLon[0] = 0;
  } else if (!strcmp(t, "GGA") && n > 7) {
    R.gpsSats = atoi(f[7]);
  }
}
static void gpsFeed() {
  while (Serial1.available()) {
    char c = Serial1.read(); R.gpsBytes++;
    if (c == '\r') continue;
    if (c == '\n') { gLine[gLen] = 0; if (gLen) nmeaLine(gLine); gLen = 0; }
    else if (gLen < (int)sizeof(gLine) - 1) gLine[gLen++] = c;
    else gLen = 0;
  }
}
// Find the GPS: each Grove pin as RX at common rates, TX unused (we never write to it).
static void gpsFind() {
  static const uint32_t bauds[] = {9600, 115200, 38400};
  static const int pins[] = {P_GROVE_A, P_GROVE_B};
  for (uint32_t b : bauds) for (int p : pins) {
    M5.Display.fillRect(0, 60, 240, 12, TFT_BLACK); M5.Display.setCursor(4, 60);
    M5.Display.printf("GPS search: RX G%d @ %lu", p, (unsigned long)b);
    Serial1.end(); Serial1.begin(b, SERIAL_8N1, p, -1);
    uint32_t good0 = R.gpsGood, t = millis();
    while (millis() - t < 1600) { gpsFeed(); delay(5); }
    if (R.gpsGood > good0) { R.gpsPin = p; R.gpsBaud = b; Serial.printf("[gps] NMEA on RX G%d @ %lu\n", p, (unsigned long)b); return; }
  }
  Serial1.end(); Serial.println("[gps] no NMEA on either Grove pin at 9600/115200/38400");
}

// ---------------- keyboard (TCA8418, polled) ----------------
static bool tcaW(uint8_t r, uint8_t v) { return M5.In_I2C.writeRegister8(TCA, r, v, 400000); }
static uint8_t tcaR(uint8_t r) { return M5.In_I2C.readRegister8(TCA, r, 400000); }
static void keysBegin() {
  R.keyOk = M5.In_I2C.scanID(TCA, 400000);
  if (!R.keyOk) { Serial.println("[key] TCA8418 not found at 0x34"); return; }
  tcaW(0x1D, 0x7F); tcaW(0x1E, 0xFF); tcaW(0x1F, 0x00);  // 7 rows, 8 columns in the matrix
  tcaW(0x01, 0x01);                                       // KE_IEN: key events into the FIFO
  for (int i = 0; i < 12 && (tcaR(0x03) & 0x0F); i++) tcaR(0x04);   // drop anything queued
  tcaW(0x02, 0x1F);
}
static void keysPoll() {
  if (!R.keyOk) return;
  int n = tcaR(0x03) & 0x0F;
  for (int i = 0; i < n; i++) {
    uint8_t ev = tcaR(0x04); if (!ev) break;
    int code = ev & 0x7F; bool down = ev & 0x80;
    R.keyCode = code; R.keyDown = down; if (down) R.keyN++;
    int row = (code - 1) / 10, col = (code - 1) % 10;
    Serial.printf("[key] %s code %d row %d col %d\n", down ? "down" : "up  ", code, row, col);
    if (down) M5.Speaker.tone(2200, 12);                  // speaker check on every press
  }
  if (n) tcaW(0x02, 0x01);
}

// ---------------- serial commands ----------------
static void report() {
  Serial.printf("[report] board %s  psram %lu  heap %u min %u  reset %d\n", R.isAdv ? "CardputerADV" : "other",
                (unsigned long)R.psram, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (int)esp_reset_reason());
  Serial.printf("[report] cc1101 %s part 0x%02X ver 0x%02X | nrf24 %s | sd %s type %u %luMB pass %lu fail %lu\n",
                R.ccOk ? "yes" : "no", R.ccPart, R.ccVer, R.nrfOk ? "yes" : "no", R.sdOk ? "yes" : "no", R.sdType,
                (unsigned long)R.sdMB, (unsigned long)R.sdPass, (unsigned long)R.sdFail);
  Serial.printf("[report] gps G%d @ %lu bytes %lu good %lu bad %lu fix %c sats %d %s %s utc %s\n", R.gpsPin,
                (unsigned long)R.gpsBaud, (unsigned long)R.gpsBytes, (unsigned long)R.gpsGood, (unsigned long)R.gpsBad,
                R.gpsFix, R.gpsSats, R.gpsLat, R.gpsLon, R.gpsUtc);
  Serial.printf("[report] imu %s %.2f %.2f %.2f | bat %d mV %d%% chg %d | keys %s presses %lu | frames %lu\n",
                R.imuOk ? "yes" : "no", R.ax, R.ay, R.az, R.batMv, R.batPct, R.chg, R.keyOk ? "yes" : "no",
                (unsigned long)R.keyN, (unsigned long)R.frames);
}
static void command(const String& c) {
  if (c == "report") report();
  else if (c == "beep") { M5.Speaker.tone(1000, 200); Serial.println("[beep] 1 kHz 200 ms"); }
  else if (c == "ir") { if (ir) { ir->sendNEC(0x00FF00FFUL, 32); Serial.println("[ir] sent one NEC frame on G44"); } }
  else if (c == "gpsraw") { gpsRaw = !gpsRaw; Serial.printf("[gps] raw %s\n", gpsRaw ? "on" : "off"); }
  else Serial.println("[help] report, beep, ir (sends one IR frame), gpsraw");
}

// ---------------- screen ----------------
static void row(int y, uint16_t col, const char* fmt, ...) {
  char b[48]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  char p[41]; snprintf(p, sizeof(p), "%-40s", b);         // pad so old text is overwritten
  M5.Display.setTextColor(col, TFT_BLACK); M5.Display.drawString(p, 0, y);
}
static void drawScreen() {
  const uint16_t A = 0xFFE0, G = TFT_GREEN, RD = TFT_RED;
  row(0,   A, "FUD HW TEST  cycle %lu  frame %lu", (unsigned long)R.cycles, (unsigned long)R.frames);
  row(9,   A, "PSRAM %luK  heap %uK", (unsigned long)(R.psram / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
  row(18,  R.ccOk ? G : RD, "CC1101 %s v%02X  433 pk%d fl%d", R.ccOk ? "ok" : "--", R.ccVer, R.pk433, R.fl433);
  row(27,  R.ccOk ? G : RD, "              915 pk%d fl%d", R.pk915, R.fl915);
  row(36,  R.nrfOk ? G : RD, "nRF24 %s  hot %d/126", R.nrfOk ? "ok" : "--", R.nrfHot);
  row(45,  R.sdOk ? G : RD, "SD %s %luMB  rw %lu/%lu", R.sdOk ? "ok" : "--", (unsigned long)R.sdMB,
      (unsigned long)R.sdPass, (unsigned long)(R.sdPass + R.sdFail));
  row(54,  A, "WiFi %d  BLE %d  (passive)", R.wifiN, R.bleN);
  row(63,  R.gpsGood ? G : RD, "GPS G%d@%lu nmea %lu fix %c sat %d", R.gpsPin, (unsigned long)R.gpsBaud,
      (unsigned long)R.gpsGood, R.gpsFix, R.gpsSats);
  row(72,  A, "  %s %s", R.gpsLat[0] ? R.gpsLat : "no position", R.gpsLon);
  row(81,  R.imuOk ? G : RD, "IMU %.2f %.2f %.2f", R.ax, R.ay, R.az);
  row(90,  A, "BAT %dmV %d%% chg %d", R.batMv, R.batPct, R.chg);
  row(99,  R.keyOk ? G : RD, "KEY %s code %d %s n=%lu", R.keyOk ? "ok" : "--", R.keyCode, R.keyDown ? "down" : "up",
      (unsigned long)R.keyN);
  row(108, A, "IR G44 ready (serial: ir)");
  int x = (int)(R.frames * 4 % 240);                      // moving block: the screen never stops
  M5.Display.fillRect(0, 124, 240, 8, TFT_BLACK); M5.Display.fillRect(x, 124, 16, 8, A);
  R.frames++;
}

void setup() {
  auto cfg = M5.config(); cfg.internal_imu = true; cfg.internal_spk = true; M5.begin(cfg);
  Serial.begin(115200); delay(300);
  R.isAdv = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
  R.psram = ESP.getPsramSize();
  Serial.printf("\n[hwtest] board %d (%s)  psram %lu  reset %d\n", (int)M5.getBoard(),
                R.isAdv ? "CardputerADV" : "NOT a Cardputer ADV", (unsigned long)R.psram, (int)esp_reset_reason());
  // M5GFX gives the ADV panel a rotation offset of 1, so its rotation 0 is already landscape.
  // (On the StickS3, 1 is landscape. Rotation is a per-board value.)
  M5.Display.setRotation(0); M5.Display.fillScreen(TFT_BLACK); M5.Display.setTextSize(1);
  Serial.printf("[hwtest] display %dx%d rotation %d\n", (int)M5.Display.width(), (int)M5.Display.height(),
                (int)M5.Display.getRotation());
  if (!R.isAdv) {                                         // wrong board: touch no pins at all
    M5.Display.setTextColor(TFT_RED, TFT_BLACK); M5.Display.drawString("Not a Cardputer ADV. Halted.", 4, 4);
    Serial.println("[hwtest] halted: this build only drives Cardputer ADV pins");
    for (;;) delay(1000);
  }
  // Color bars: panel, colors, orientation.
  const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, 0xFFE0, TFT_BLACK};
  for (int i = 0; i < 6; i++) M5.Display.fillRect(i * 40, 0, 40, 135, bars[i]);
  delay(800); M5.Display.fillScreen(TFT_BLACK);
  M5.Speaker.setVolume(80); M5.Speaker.tone(1400, 60);

  // Every CS high before the bus starts: the Hydra isolates whichever radio is not selected.
  pinMode(P_CC_CS, OUTPUT); digitalWrite(P_CC_CS, HIGH);
  pinMode(P_NRF_CSN, OUTPUT); digitalWrite(P_NRF_CSN, HIGH);
  pinMode(P_SD_CS, OUTPUT); digitalWrite(P_SD_CS, HIGH);
  pinMode(P_NRF_CE, OUTPUT); digitalWrite(P_NRF_CE, LOW);
  pinMode(P_CC_GDO0, INPUT);
  bus.begin(P_SCK, P_MISO, P_MOSI, -1);

  ccProbe(); nrfProbe(); sdMount();
  Serial.printf("[hwtest] cc1101 %s (part 0x%02X ver 0x%02X)  nrf24 %s  sd %s (%luMB)\n", R.ccOk ? "yes" : "no",
                R.ccPart, R.ccVer, R.nrfOk ? "yes" : "no", R.sdOk ? "yes" : "no", (unsigned long)R.sdMB);
  keysBegin();
  ir = new IRsend(P_IR); ir->begin();                     // pin set up, nothing sent
  WiFi.mode(WIFI_OFF);
  NimBLEDevice::init("");
  gpsFind();
  M5.Display.fillScreen(TFT_BLACK);
  xTaskCreatePinnedToCore(busTask, "bus", 8192, nullptr, 1, nullptr, 0);
  Serial.println("[hwtest] running. serial: report, beep, ir, gpsraw");
}

void loop() {
  static String line; static uint32_t lastDraw = 0, lastRep = 0, lastSlow = 0;
  M5.update();
  keysPoll();
  if (R.gpsPin >= 0) gpsFeed();
  uint32_t now = millis();
  if (now - lastSlow > 250) {
    lastSlow = now;
    R.imuOk = M5.Imu.getAccel(&R.ax, &R.ay, &R.az);
    R.batMv = M5.Power.getBatteryVoltage(); R.batPct = M5.Power.getBatteryLevel(); R.chg = (int)M5.Power.isCharging();
  }
  if (now - lastDraw >= 40) { lastDraw = now; drawScreen(); }   // ~25 fps, all the time
  if (now - lastRep > 10000) { lastRep = now; report(); }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { line.trim(); if (line.length()) command(line); line = ""; }
    else line += c;
  }
  delay(2);
}
#endif
