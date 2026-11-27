// Wall clock, honest about how it was set. The idea of a clock that says whether it is set,
// trusted or guessed comes from SquachWatch-CYD (Talking Sasquach); this code is our own, a
// trimmed form of FUD-Deck's clock.
//
// UTC only. GPS sets it on the first valid fix and again every hour (boards with a Grove GPS).
// A soft reset keeps the ESP32's system time, so the clock stays set but is marked "kept"
// until GPS confirms it. Until it is set, log stamps are time since boot ("+123s").
#pragma once
#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

namespace clk {

enum Source : uint8_t { NONE, KEPT, GPS };
static Source source = NONE;
static uint32_t lastGpsSet = 0;                          // millis of the last GPS set
static const uint32_t FLOOR = 1735689600UL;              // 2025-01-01: anything earlier is unset
static const uint32_t RESYNC_MS = 60UL * 60 * 1000;      // GPS re-sets the clock hourly

static uint32_t epochRaw() { return (uint32_t)time(nullptr); }
static bool isSet() { return epochRaw() >= FLOOR; }
static bool trusted() { return isSet() && source == GPS; }
static const char* sourceName() { return !isSet() ? "boot" : source == GPS ? "gps" : "kept"; }

// Days since 1970-01-01 for a civil date (Howard Hinnant's public-domain algorithm).
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2; const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
// From NMEA RMC fields: hhmmss[.ss] and ddmmyy. 0 when malformed.
static uint32_t epochFromNmea(const char* hms, const char* dmy) {
  if (strlen(hms) < 6 || strlen(dmy) < 6) return 0;
  auto two = [](const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); };
  for (int i = 0; i < 6; i++) if (!isdigit((unsigned char)hms[i]) || !isdigit((unsigned char)dmy[i])) return 0;
  int h = two(hms), mi = two(hms + 2), s = two(hms + 4), d = two(dmy), mo = two(dmy + 2), y = 2000 + two(dmy + 4);
  if (h > 23 || mi > 59 || s > 60 || d < 1 || d > 31 || mo < 1 || mo > 12) return 0;
  return (uint32_t)daysFromCivil(y, mo, d) * 86400UL + h * 3600UL + mi * 60UL + s;
}

static void begin() {
  if (isSet()) { source = KEPT; Serial.println("[clock] kept across reset (until GPS confirms)"); }
}
// Called with a valid-fix RMC time. ageMs = how long ago that sentence arrived.
static void fromGps(uint32_t epoch, uint32_t ageMs) {
  if (epoch < FLOOR) return;
  if (source == GPS && millis() - lastGpsSet < RESYNC_MS) return;
  epoch += ageMs / 1000;
  timeval tv = {(time_t)epoch, (suseconds_t)((ageMs % 1000) * 1000)};
  settimeofday(&tv, nullptr);
  bool first = source != GPS; source = GPS; lastGpsSet = millis();
  if (first) { time_t e = epoch; tm t; gmtime_r(&e, &t);
    Serial.printf("[clock] set by GPS: %04d-%02d-%02dT%02d:%02d:%02dZ\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec); }
}
// "2026-11-20 21:03:12" (UTC) for logs, or "+123s" since boot when unset.
static void stamp(char* out, size_t n) {
  if (!isSet()) { snprintf(out, n, "+%lus", (unsigned long)(millis() / 1000)); return; }
  time_t e = epochRaw(); tm t; gmtime_r(&e, &t);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}
// "21:03:12" or "--:--:--"
static void hms(char* out, size_t n) {
  if (!isSet()) { snprintf(out, n, "--:--:--"); return; }
  time_t e = epochRaw(); tm t; gmtime_r(&e, &t);
  snprintf(out, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

}  // namespace clk
