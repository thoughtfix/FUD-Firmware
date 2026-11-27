// GPS on the Grove port (Cardputer ADV). Listen only: nothing is ever written to the module.
// Finds it on its own: each Grove pin as RX at 115200, 9600 and 38400, 1.6 s per try, without
// blocking the loop. Once NMEA with a good checksum arrives it stays put. If the data stops for
// 15 s it starts looking again. Parses RMC (time, date, fix, position, speed) and GGA (quality,
// satellites in use, HDOP, altitude). Positions are decimal degrees.
#pragma once
#include <Arduino.h>
#include "clock.h"

namespace gps {

struct State {
  bool enabled = false;       // board has a Grove UART to listen on
  bool found = false;         // NMEA seen on the current pin and rate
  int8_t pin = -1; uint32_t baud = 0;
  uint32_t bytes = 0, good = 0, bad = 0, lastGood = 0, lastFixMs = 0;
  char status = '-';          // RMC: A = valid fix, V = no fix
  uint8_t quality = 0;        // GGA: 0 none, 1 GPS, 2 DGPS, ...
  int sats = -1; float hdop = 0, altM = 0, kmh = 0;
  double lat = 0, lon = 0; bool hasPos = false;
  char utc[9] = "", date[9] = "";
};
static State S;

static int8_t candPins[2] = {-1, -1};
static const uint32_t CAND_BAUD[3] = {115200, 9600, 38400};
static int cand = 0; static uint32_t candT = 0, candGood0 = 0;
static char line[100]; static int len = 0;

static void tryCand() {
  S.pin = candPins[cand % 2]; S.baud = CAND_BAUD[(cand / 2) % 3];
  Serial1.end(); Serial1.begin(S.baud, SERIAL_8N1, S.pin, -1);   // TX unused: listen only
  candT = millis(); candGood0 = S.good; len = 0;
}
static void begin(int8_t pinA, int8_t pinB) {
  if (pinA < 0) return;
  candPins[0] = pinA; candPins[1] = pinB < 0 ? pinA : pinB;
  S.enabled = true; cand = 0; tryCand();
}

static bool sumOk(const char* s) {
  if (s[0] != '$') return false;
  const char* star = strchr(s, '*'); if (!star || strlen(star) < 3) return false;
  uint8_t c = 0; for (const char* p = s + 1; p < star; p++) c ^= (uint8_t)*p;
  return c == (uint8_t)strtol(star + 1, nullptr, 16);
}
static double nmeaDeg(const char* v, const char* hemi) {   // ddmm.mmmm -> decimal degrees
  if (!v[0]) return 0;
  double x = atof(v); int d = (int)(x / 100); double m = x - d * 100;
  double r = d + m / 60.0;
  return (hemi[0] == 'S' || hemi[0] == 'W') ? -r : r;
}
static void sentence(char* s) {
  if (!sumOk(s)) { S.bad++; return; }
  S.good++; S.lastGood = millis();
  char* f[20]; int n = 0; f[n++] = s;
  for (char* p = s; *p && n < 20; p++) if (*p == ',' || *p == '*') { *p = 0; f[n++] = p + 1; }
  if (strlen(f[0]) < 6) return;
  const char* t = f[0] + 3;                                  // $GPRMC, $GNRMC, ...
  if (!strcmp(t, "RMC") && n > 9) {
    if (strlen(f[1]) >= 6) snprintf(S.utc, sizeof S.utc, "%.2s:%.2s:%.2s", f[1], f[1] + 2, f[1] + 4);
    if (strlen(f[9]) >= 6) snprintf(S.date, sizeof S.date, "%.2s/%.2s/%.2s", f[9] + 4, f[9] + 2, f[9]);
    S.status = f[2][0] ? f[2][0] : '-';
    if (S.status == 'A' && f[3][0] && f[5][0]) {
      S.lat = nmeaDeg(f[3], f[4]); S.lon = nmeaDeg(f[5], f[6]); S.hasPos = true; S.lastFixMs = millis();
      S.kmh = atof(f[7]) * 1.852f;
      clk::fromGps(clk::epochFromNmea(f[1], f[9]), 0);   // a valid fix carries trusted UTC
    }
  } else if (!strcmp(t, "GGA") && n > 9) {
    S.quality = (uint8_t)atoi(f[6]); S.sats = atoi(f[7]); S.hdop = atof(f[8]); S.altM = atof(f[9]);
  }
}
static void poll() {
  if (!S.enabled) return;
  while (Serial1.available()) {
    char c = Serial1.read(); S.bytes++;
    if (c == '\r') continue;
    if (c == '\n') { line[len] = 0; if (len) sentence(line); len = 0; }
    else if (len < (int)sizeof(line) - 1) line[len++] = c;
    else len = 0;
  }
  uint32_t now = millis();
  if (!S.found) {
    if (S.good > candGood0) { S.found = true; Serial.printf("[gps] NMEA on Grove G%d @ %lu\n", S.pin, (unsigned long)S.baud); }
    else if (now - candT > 1600) { cand = (cand + 1) % 6; tryCand(); }
  } else if (now - S.lastGood > 15000) {
    S.found = false; Serial.println("[gps] data stopped, searching"); cand = (cand + 1) % 6; tryCand();
  }
}
// A fix that is current: RMC said A within the last 5 s.
static bool fixed() { return S.found && S.status == 'A' && S.hasPos && millis() - S.lastFixMs < 5000; }

}  // namespace gps
