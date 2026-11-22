// Wardriving: every Wi-Fi network the normal scan hears, with the GPS position, saved to the SD
// card as a WiGLE CSV (format 1.4). Boards with an SD slot and a Grove GPS (Cardputer ADV).
//
// Opt-in: OFF by default, and turning it ON shows the privacy warning first (Settings >
// Wardrive). Nothing extra is transmitted: it records the scan that already runs, passive while
// Silent is ON. Nothing is uploaded; the owner takes the card to WiGLE (or not).
// A row is written only with a current GPS fix and a GPS-set clock (WiGLE needs both).
// Each network once per session (by BSSID). One file per session: /fud/wigle_YYYYMMDD_HHMMSS.csv.
//
// The scan task captures records during the Wi-Fi phase (capture) and writes them after the
// next BLE phase (service), the same slot as the detection log. The record buffer exists only
// while wardriving is ON. Included from main.cpp after sdlog.h and gps.h.
#pragma once

namespace wd {

// Position in microdegrees: exact to the 6 decimals written, half the size of doubles.
struct Rec { uint8_t mac[6]; char ssid[33]; uint8_t auth, ch; int8_t rssi; uint32_t epoch; int32_t lat, lon; int16_t alt; uint16_t acc; };
static const int WN = 64, SEEN_N = 512;   // ~3.8 KB ring + 2 KB seen, only while ON
struct Buf { Rec ring[WN]; uint32_t seen[SEEN_N]; };   // seen: BSSID hashes this session (0 = empty)

static bool avail = false, on = false;
static Buf* buf = nullptr;
static volatile int qHead = 0, qTail = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char path[40] = "";
static uint32_t written = 0, dropped = 0, seenCount = 0;

static int queued() { return (qHead - qTail + WN) % WN; }
static void begin() {
  avail = sdlog::avail && B->groveA >= 0;
  on = avail && prefs.getBool("wardrive", false);
}
static void setOn(bool v) { on = v && avail; prefs.putBool("wardrive", on); }

static uint32_t macHash(const uint8_t* m) {
  uint32_t h = 2166136261u; for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; } return h ? h : 1;
}
// true if new this session (and now remembered). Open addressing; when nearly full, start over.
static bool firstSight(const uint8_t* m) {
  uint32_t h = macHash(m);
  if (seenCount > SEEN_N * 3 / 4) { memset(buf->seen, 0, sizeof(buf->seen)); seenCount = 0; }
  for (int k = 0; k < SEEN_N; k++) {
    uint32_t& s = buf->seen[(h + k) % SEEN_N];
    if (s == h) return false;
    if (!s) { s = h; seenCount++; return true; }
  }
  return false;
}

// Scan task, Wi-Fi phase: one scan result. Cheap: no card access.
static void capture(const wifi_ap_record_t* r) {
  if (!on || !buf || !r || !gps::fixed() || !clk::trusted()) return;
  if ((qHead + 1) % WN == qTail) { dropped++; return; }     // full: not marked seen, so a later scan retries
  if (!firstSight(r->bssid)) return;
  Rec x{};
  memcpy(x.mac, r->bssid, 6); memcpy(x.ssid, r->ssid, 32); x.ssid[32] = 0;
  x.auth = (uint8_t)r->authmode; x.ch = r->primary; x.rssi = r->rssi; x.epoch = clk::epochRaw();
  x.lat = (int32_t)lround(gps::S.lat * 1e6); x.lon = (int32_t)lround(gps::S.lon * 1e6); x.alt = (int16_t)gps::S.altM;
  x.acc = (uint16_t)(gps::S.hdop > 0 ? gps::S.hdop * 5.0f + 0.5f : 50);   // rough: HDOP x 5 m
  portENTER_CRITICAL(&mux);
  int nx = (qHead + 1) % WN;
  if (nx == qTail) dropped++; else { buf->ring[qHead] = x; qHead = nx; }
  portEXIT_CRITICAL(&mux);
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

// Scan task, after BLE: allocate or free the buffer to match the setting, then write what is queued.
static void service() {
  if (!avail) return;
  if (on && !buf) {
    buf = (Buf*)calloc(1, sizeof(Buf));
    qHead = qTail = 0; seenCount = 0; path[0] = 0;
    if (!buf) { Serial.println("[wardrive] no memory, off"); on = false; }
    return;
  }
  if (!on && buf) { if (!queued()) { free(buf); buf = nullptr; } else return; }   // flush first, then free
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
    f.printf("WigleWifi-1.4,appRelease=%s,model=%s,release=%s,device=FUD,display=FUD,board=ESP32-S3,brand=M5Stack\n",
             FUD_VERSION, B->name, FUD_VERSION);
    f.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  }
  while (queued()) {
    const Rec& x = buf->ring[qTail];
    char ss[70]; int j = 0; ss[j++] = '"';                       // quote the SSID, double any quote
    for (int i = 0; x.ssid[i] && j < 66; i++) { char c = x.ssid[i]; if (c == '"') ss[j++] = '"'; ss[j++] = (c < 0x20) ? ' ' : c; }
    ss[j++] = '"'; ss[j] = 0;
    time_t e = (time_t)x.epoch; tm t; gmtime_r(&e, &t);
    auto deg = [](char* o, size_t n, int32_t u) { snprintf(o, n, "%s%ld.%06ld", u < 0 ? "-" : "", (long)(labs(u) / 1000000), (long)(labs(u) % 1000000)); };
    char la[16], lo[16]; deg(la, sizeof la, x.lat); deg(lo, sizeof lo, x.lon);
    f.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%04d-%02d-%02d %02d:%02d:%02d,%d,%d,%s,%s,%d,%u,WIFI\n",
             x.mac[0], x.mac[1], x.mac[2], x.mac[3], x.mac[4], x.mac[5], ss, authStr(x.auth),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, x.ch, x.rssi, la, lo, (int)x.alt, (unsigned)x.acc);
    portENTER_CRITICAL(&mux); qTail = (qTail + 1) % WN; portEXIT_CRITICAL(&mux);
    written++;
  }
  f.close();
  sdlog::unmount();
}

}  // namespace wd
