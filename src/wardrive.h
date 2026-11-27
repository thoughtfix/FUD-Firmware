// Wardriving: Toys > Wardrive on boards with an SD slot and a Grove GPS (Cardputer ADV). Every
// Wi-Fi network and Bluetooth LE device heard, with the GPS position, saved to the SD card as a
// WiGLE CSV (WigleWifi-1.6, the layout SquachWatch-CYD writes and wigle.net accepts as is; this
// code is ours).
//
// A mode, not a background setting: it runs only while the Wardrive screen is open, after the
// privacy warning, and stops (file flushed) when you leave. Nothing extra is transmitted: the
// scans stay passive while Silent is ON. Nothing is uploaded; the owner takes the card to WiGLE.
// A row is written only with a current GPS fix and a GPS-set clock (WiGLE needs both).
// Each device once per session (by MAC). One file per session: /fud/wigle_YYYYMMDD_HHMMSS.csv.
//
// The scan task captures records during its scans (capture, captureBle) and writes them between
// scans (service), with Wi-Fi off. The record buffer exists only while the mode is open.
// Included from main.cpp after sdlog.h and gps.h.
#pragma once

namespace wd {

enum : uint8_t { K_WIFI = 0, K_BLE = 1 };
// Position in microdegrees: exact to the 6 decimals written, half the size of doubles.
struct Rec { uint8_t mac[6]; char name[33]; uint8_t kind, auth, ch; int8_t rssi; uint16_t mfgr; bool hasMfgr;
             uint32_t epoch; int32_t lat, lon; int16_t alt; uint16_t acc; };
static const int WN = 96, SEEN_N = 1024;   // ~6 KB ring + 4 KB seen, only while the mode is open
struct Buf { Rec ring[WN]; uint32_t seen[SEEN_N]; };   // seen: MAC hashes this session (0 = empty)

static bool avail = false, on = false;
static Buf* buf = nullptr;
static volatile int qHead = 0, qTail = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char path[40] = "";
static uint32_t written = 0, dropped = 0, seenCount = 0;
// Live counts for the Wardrive screen (this session).
static volatile uint32_t nWifi = 0, nBle = 0;
static uint32_t startMs = 0;
static const int MINUTE_BINS = 6;                       // new devices in the last minute, 10 s bins
static volatile uint16_t bins[MINUTE_BINS]; static volatile uint32_t binAt = 0;
static char lastName[33] = "";

static int queued() { return (qHead - qTail + WN) % WN; }
static void begin() { avail = sdlog::avail && B->groveA >= 0; on = false; }
static void setOn(bool v) {
  v = v && avail; if (v == on) return;
  on = v;
  if (v) { nWifi = nBle = 0; startMs = millis(); for (int i = 0; i < MINUTE_BINS; i++) bins[i] = 0; binAt = millis() / 10000; lastName[0] = 0; }
}
static uint32_t lastMinute() {
  uint32_t now = millis() / 10000, s = 0;                // bins for the 10 s slots now-5 .. now
  for (uint32_t k = 0; k < (uint32_t)MINUTE_BINS; k++) { uint32_t slot = now - k;
    if (slot <= binAt && binAt - slot < (uint32_t)MINUTE_BINS) s += bins[slot % MINUTE_BINS]; }
  return s;
}

static uint32_t macHash(const uint8_t* m, uint8_t kind) {
  uint32_t h = 2166136261u ^ kind; for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; } return h ? h : 1;
}
// true if new this session (and now remembered). Open addressing; when nearly full, start over.
static bool firstSight(const uint8_t* m, uint8_t kind) {
  uint32_t h = macHash(m, kind);
  if (seenCount > SEEN_N * 3 / 4) { memset(buf->seen, 0, sizeof(buf->seen)); seenCount = 0; }
  for (int k = 0; k < SEEN_N; k++) {
    uint32_t& s = buf->seen[(h + k) % SEEN_N];
    if (s == h) return false;
    if (!s) { s = h; seenCount++; return true; }
  }
  return false;
}
static bool ready() { return on && buf && gps::fixed() && clk::trusted(); }
static bool room() { if ((qHead + 1) % WN == qTail) { dropped++; return false; } return true; }   // full: not marked seen, retried
static void fillPos(Rec& x) {
  x.epoch = clk::epochRaw();
  x.lat = (int32_t)lround(gps::S.lat * 1e6); x.lon = (int32_t)lround(gps::S.lon * 1e6); x.alt = (int16_t)gps::S.altM;
  x.acc = (uint16_t)(gps::S.hdop > 0 ? gps::S.hdop * 5.0f + 0.5f : 50);   // rough: HDOP x 5 m
}
static void push(const Rec& x) {
  portENTER_CRITICAL(&mux);
  int nx = (qHead + 1) % WN;
  if (nx == qTail) dropped++; else { buf->ring[qHead] = x; qHead = nx; }
  portEXIT_CRITICAL(&mux);
  uint32_t b = millis() / 10000;                        // count it for "new in the last minute"
  if (b != binAt) { for (uint32_t i = binAt + 1; i <= b && i - binAt <= (uint32_t)MINUTE_BINS; i++) bins[i % MINUTE_BINS] = 0; binAt = b; }
  bins[b % MINUTE_BINS]++;
  if (x.name[0]) { memcpy(lastName, x.name, sizeof lastName); }
}

// Scan task, Wi-Fi scan: one result. Cheap: no card access.
static void capture(const wifi_ap_record_t* r) {
  if (!r || !ready() || !room() || !firstSight(r->bssid, K_WIFI)) return;
  Rec x{}; x.kind = K_WIFI;
  memcpy(x.mac, r->bssid, 6); memcpy(x.name, r->ssid, 32); x.name[32] = 0;
  x.auth = (uint8_t)r->authmode; x.ch = r->primary; x.rssi = r->rssi; fillPos(x);
  push(x); nWifi++;
}
// Scan task, BLE scan: one advertiser (name and manufacturer ID when it sends them).
static void captureBle(const uint8_t* mac, const char* name, int8_t rssi, int mfgr) {
  if (!ready() || !room() || !firstSight(mac, K_BLE)) return;
  Rec x{}; x.kind = K_BLE;
  for (int i = 0; i < 6; i++) x.mac[i] = mac[5 - i];    // NimBLE keeps the address little-endian
  if (name) { strncpy(x.name, name, 32); x.name[32] = 0; }
  x.rssi = rssi; x.hasMfgr = mfgr >= 0; x.mfgr = mfgr >= 0 ? (uint16_t)mfgr : 0; fillPos(x);
  push(x); nBle++;
}

static const char* authStr(uint8_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "[ESS]";
    case WIFI_AUTH_WEP: return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK: return "[WPA-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA2_PSK: return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK: return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP-CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK: return "[WPA3-SAE-CCMP][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    default: return "[ESS]";
  }
}
static int chanMHz(uint8_t ch) { return ch == 14 ? 2484 : (ch >= 1 && ch <= 13) ? 2407 + 5 * ch : 0; }

// Scan task, between scans with Wi-Fi off: allocate or free the buffer to match the mode, then
// write what is queued.
static void service() {
  if (!avail) return;
  if (on && !buf) {
    buf = (Buf*)calloc(1, sizeof(Buf));
    qHead = qTail = 0; seenCount = 0; path[0] = 0;
    if (!buf) { Serial.println("[wardrive] no memory, off"); on = false; }
    return;
  }
  if (!on && buf) { if (!queued()) { free(buf); buf = nullptr; } }   // flush first, then free
  if (!buf || !queued()) return;
  if (!sdlog::cardOk && millis() - sdlog::lastTry < 30000) return;
  if (!sdlog::mount()) return;
  if (!path[0]) {   // new session file, named by the UTC time of its first row
    time_t e = (time_t)buf->ring[qTail].epoch; tm t; gmtime_r(&e, &t);
    snprintf(path, sizeof(path), "%s/wigle_%04d%02d%02d_%02d%02d%02d.csv", sdlog::DIR,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  }
  File f = SD.open(path, FILE_APPEND);
  if (!f) { Serial.println("[wardrive] write failed, will retry"); sdlog::cardOk = false; sdlog::unmount(); return; }
  if (f.size() == 0) {
    f.printf("WigleWifi-1.6,appRelease=%s,model=%s,release=%s,device=FUD,display=FUD,board=ESP32-S3,brand=M5Stack,star=Sol,body=3,subBody=0\n",
             FUD_VERSION, B->name, FUD_VERSION);
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  }
  while (queued()) {
    const Rec& x = buf->ring[qTail];
    char ss[70]; int j = 0;                                     // quote names with , or ", double any quote
    bool q = false; for (int i = 0; x.name[i]; i++) if (x.name[i] == ',' || x.name[i] == '"') q = true;
    if (q) ss[j++] = '"';
    for (int i = 0; x.name[i] && j < 66; i++) { char c = x.name[i]; if (c == '"') ss[j++] = '"'; ss[j++] = (c < 0x20) ? ' ' : c; }
    if (q) ss[j++] = '"';
    ss[j] = 0;
    time_t e = (time_t)x.epoch; tm t; gmtime_r(&e, &t);
    auto deg = [](char* o, size_t n, int32_t u) { snprintf(o, n, "%s%ld.%06ld", u < 0 ? "-" : "", (long)(labs(u) / 1000000), (long)(labs(u) % 1000000)); };
    char la[16], lo[16]; deg(la, sizeof la, x.lat); deg(lo, sizeof lo, x.lon);
    char mac[18]; snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", x.mac[0], x.mac[1], x.mac[2], x.mac[3], x.mac[4], x.mac[5]);
    char ts[20]; snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    if (x.kind == K_WIFI)
      f.printf("%s,%s,%s,%s,%d,%d,%d,%s,%s,%d,%u,,,WIFI\n", mac, ss, authStr(x.auth), ts, x.ch, chanMHz(x.ch), x.rssi, la, lo, (int)x.alt, (unsigned)x.acc);
    else if (x.hasMfgr)
      f.printf("%s,%s,Misc [LE],%s,0,,%d,%s,%s,%d,%u,,%u,BLE\n", mac, ss, ts, x.rssi, la, lo, (int)x.alt, (unsigned)x.acc, (unsigned)x.mfgr);
    else
      f.printf("%s,%s,Misc [LE],%s,0,,%d,%s,%s,%d,%u,,,BLE\n", mac, ss, ts, x.rssi, la, lo, (int)x.alt, (unsigned)x.acc);
    portENTER_CRITICAL(&mux); qTail = (qTail + 1) % WN; portEXIT_CRITICAL(&mux);
    written++;
  }
  f.close();
  sdlog::unmount();
}

}  // namespace wd
