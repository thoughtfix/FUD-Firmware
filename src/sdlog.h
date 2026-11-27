// SD card detection log (boards with an SD slot: Cardputer ADV). The CSV layout and the
// no-clock fallback follow FUD-Deck's sdlog and FUD-X3's Logger (thoughtfix, GPL-3.0).
//
// Every new detection becomes one CSV line:   timestamp,category,label,name,mac,rssi,conf
// Clock set by GPS: a file per UTC day, /fud/20261120.csv, stamped "2026-11-20 21:03:12" (UTC).
// No clock yet: one file per boot, /fud/boot0007.csv, stamped in seconds since boot ("+123s").
//
// The card shares the SPI bus with the Hydra radios, which only the scan task drives. So the
// UI side only queues lines (note), and the scan task writes them (service) between its own
// radio phases, after BLE and before Wi-Fi: never during a radio's use, and never at the
// Wi-Fi heap low point. The card is mounted only for each write and unmounted after: a mounted
// FAT volume holds ~20 KB of buffers, too much to keep on a board without PSRAM.
// No card or a failed write = no logging and no hang; it retries every 30 s.
// Included from main.cpp after rfspi, Det, clk and prefs.
#pragma once
#include <SD.h>

namespace sdlog {

static const char* DIR = "/fud";
static const char* HEADER = "timestamp,category,label,name,mac,rssi,conf";
static const int QN = 32, QW = 112;              // ring of queued lines

static bool avail = false;                       // board has a slot
static bool on = true;                           // Settings > SD Log (NVS "sdlog")
static bool mounted = false, cardOk = false;     // cardOk: the last mount worked
static char path[32] = "";
static uint32_t written = 0, dropped = 0, lastTry = 0, lastOkMs = 0;
static uint16_t bootNo = 0;
static char ring[QN][QW]; static volatile int qHead = 0, qTail = 0;   // note() writes head, service() reads tail
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int tailReq = 0;                 // bench: print the last N lines on the next service
static const char* volatile tailPath = nullptr;  // which file (nullptr = the detection log)
static volatile bool lsReq = false;              // bench: list /fud on the next service
static const char* volatile rmPrefix = nullptr;  // bench: with lsReq, delete /fud files starting with this

static bool present() { return cardOk; }
static int queued() { return (qHead - qTail + QN) % QN; }

// Card off the bus: CS high, then a couple of clocks so it lets go of MISO for the radios.
static void release() {
  digitalWrite(B->sdCs, HIGH);
  rfspi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  rfspi.transfer(0xFF); rfspi.transfer(0xFF);
  rfspi.endTransaction();
}
static void choosePath() {
  if (clk::trusted()) {
    time_t e = (time_t)clk::epochRaw(); tm t; gmtime_r(&e, &t);
    snprintf(path, sizeof(path), "%s/%04d%02d%02d.csv", DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  } else snprintf(path, sizeof(path), "%s/boot%04u.csv", DIR, bootNo);
}
static bool mount() {
  lastTry = millis();
  bool ok = SD.begin(B->sdCs, rfspi, 20000000, "/sd", 2);
  if (!ok) { SD.end(); ok = SD.begin(B->sdCs, rfspi, 4000000, "/sd", 2); }
  if (ok) { if (!SD.exists(DIR)) SD.mkdir(DIR); choosePath(); }
  else SD.end();
  release();
  if (ok != cardOk || !lastOkMs) Serial.printf("[sdlog] %s %s\n", ok ? "logging to" : "no card or it will not mount", ok ? path : "");
  mounted = cardOk = ok; if (ok) lastOkMs = millis();
  return ok;
}
static void unmount() { SD.end(); release(); mounted = false; }

static void setOn(bool v) { on = v; prefs.putBool("sdlog", v); }

// Before the scan task starts (nothing else is on the bus yet).
static void begin() {
  avail = B->known && B->sdCs >= 0;
  if (!avail) return;
  on = prefs.getBool("sdlog", true);
  bootNo = prefs.getUShort("sdboot", 0) + 1; prefs.putUShort("sdboot", bootNo);
  if (on && mount()) unmount();   // check the card now, so Settings can say NO CARD
}

// UI side (ingest): queue one line for a new detection. Never touches the card.
static void note(const Det& d) {
  if (!avail || !on) return;
  char name[33]; int j = 0;
  for (int i = 0; d.name[i] && j < 32; i++) { char c = d.name[i]; name[j++] = (c == ',' || c == '"' || c < 0x20) ? ' ' : c; }
  name[j] = 0;
  char ts[24]; clk::stamp(ts, sizeof(ts));
  char line[QW];
  snprintf(line, sizeof(line), "%s,%s,%s,%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%c", ts, catTag[d.cat], d.label, name,
           d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5], d.rssi, d.conf);
  portENTER_CRITICAL(&mux);
  int nx = (qHead + 1) % QN;
  if (nx == qTail) dropped++;                    // full: drop rather than block
  else { memcpy(ring[qHead], line, QW); qHead = nx; }
  portEXIT_CRITICAL(&mux);
}

static void printTail(int n, const char* p) {
  File f = SD.open(p, FILE_READ);
  if (!f) { Serial.printf("[sdlog] cannot open %s\n", p); return; }
  size_t sz = f.size();
  f.seek(sz > (size_t)n * 90 ? sz - n * 90 : 0);
  Serial.printf("[sdlog] %s, %u bytes:\n", p, (unsigned)sz);
  while (f.available()) { String l = f.readStringUntil('\n'); Serial.printf("  %s\n", l.c_str()); }
  f.close();
}

// Scan task, between radio phases: write what is queued.
static void service() {
  if (!avail) return;
  if (!on || (!queued() && !tailReq && !lsReq)) return;
  if (!cardOk && millis() - lastTry < 30000) return;   // no card: retry every 30 s
  if (!mount()) return;
  if (queued()) {
    choosePath();                                // clock just set, or a new UTC day
    File f = SD.open(path, FILE_APPEND);
    if (!f) { Serial.println("[sdlog] write failed, will retry"); cardOk = false; unmount(); return; }   // lines stay queued
    if (f.size() == 0) f.println(HEADER);
    char line[QW];
    while (queued()) {
      memcpy(line, ring[qTail], QW);             // only service() moves the tail
      if (!f.println(line)) break;
      portENTER_CRITICAL(&mux); qTail = (qTail + 1) % QN; portEXIT_CRITICAL(&mux);
      written++;
    }
    f.close();
  }
  if (lsReq) { lsReq = false; File d = SD.open(DIR); int n = 0;
    const char* rp = rmPrefix; rmPrefix = nullptr; char del[24][40]; int nd = 0;
    if (d) { for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        if (rp && !strncmp(e.name(), rp, strlen(rp)) && nd < 24) snprintf(del[nd++], 40, "%s/%s", DIR, e.name());
        else { Serial.printf("[sdls] %8u  %s\n", (unsigned)e.size(), e.name()); n++; }
        e.close(); } d.close(); }
    for (int i = 0; i < nd; i++) Serial.printf("[sdls] deleted %s: %s\n", del[i], SD.remove(del[i]) ? "ok" : "FAILED");
    Serial.printf("[sdls] %d files in %s\n", n, DIR); }
  if (tailReq) { printTail(tailReq, tailPath && tailPath[0] ? tailPath : path); tailReq = 0; tailPath = nullptr; }
  unmount();
}

}  // namespace sdlog
