// Serial telemetry for the bench. Included once from main.cpp, after the globals
// it reads (dets, counts, mode, MODE_NAMES, Evt/evtQ). One job, one file, so it
// lifts out cleanly: remove the include and the telem::poll() call in loop().
//
// Read-only and always compiled: state, shot, ver, help. These never transmit and
// never change detection state, so they are safe in the release env.
//   state   machine-readable dump of the current detections (the detection oracle
//           for the RTE bench: after RTE emits a fixture, poll this and look for the
//           expected cat= line)
//   shot    screen as RGB332 rows, same format as FUD-Deck's dev build and probe.py.
//           Best-effort: blank if the panel is write-only.
//   ver     firmware version line
//   help    command list
//
// Detection injection is bench-only, behind FUD_BENCH (env sticks3-dev):
//   fake <tracker|camera|drone|subghz|hackdev|attack|jam> [keybyte]
//           enqueues a made-up detection so the alert + telemetry path can be tested
//           with no radios and no transmitting. Local UI state only, nothing is sent.
#pragma once

namespace telem {

static const char* MODE_NAMES[] = {
  "idle","status","zoom","list","item","attack","waterfall","settings",
  "toys","ssid","traffic","tvbg","finder"
};

static String g_line;
static bool readLine(String& out) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { out = g_line; g_line = ""; return true; }
    if (g_line.length() < 120) g_line += c;
  }
  return false;
}

static void dumpState() {
  Serial.println("[state] BEGIN");
  Serial.printf("[state] fw %s screen=%s radio=%c muted=%d cc=%d nrf=%d\n",
                FUD_VERSION, MODE_NAMES[mode], activeRadio, muted ? 1 : 0,
                ccPresent ? 1 : 0, nrfPresent ? 1 : 0);
  Serial.printf("[state] counts cam=%lu drn=%lu hak=%lu atk=%lu trk=%lu sub=%lu\n",
                (unsigned long)cntCam, (unsigned long)cntDrn, (unsigned long)cntHak,
                (unsigned long)cntAtk, (unsigned long)cntTrk, (unsigned long)cntSub);
  for (int i = 0; i < MAXDET; i++) {
    Det& d = dets[i];
    if (!d.used) continue;
    Serial.printf("[state] det %d cat=%s conf=%c rssi=%d hits=%u age=%lus alarm=%d acked=%d ssid=%d active=%d "
                  "key=%02X:%02X:%02X:%02X:%02X:%02X label=\"%s\" name=\"%s\"\n",
                  i, catTag[d.cat], d.conf, d.rssi, d.hits,
                  (unsigned long)((millis() - d.firstSeen) / 1000),
                  d.alarm ? 1 : 0, d.acked ? 1 : 0, d.ssidBased ? 1 : 0, d.active ? 1 : 0,
                  d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5],
                  d.label, d.name);
  }
  Serial.println("[state] END");
}

static void dumpShot() {
  int w = M5.Display.width(), h = M5.Display.height();
  static uint16_t row[240];
  static char hex[2 * 240 + 16];
  static const char* hx = "0123456789ABCDEF";
  if (w > 240) w = 240;
  Serial.printf("[shot] BEGIN %d %d %d\n", w, h, (int)mode);
  Serial.flush();
  for (int y = 0; y < h; y++) {
    M5.Display.readRect(0, y, w, 1, row);
    int n = snprintf(hex, sizeof(hex), "R%d:", y);
    for (int x = 0; x < w; x++) {
      uint16_t c = row[x];
      c = (uint16_t)((c >> 8) | (c << 8));   // readRect returns the panel's big-endian 16-bit words; swap to RGB565
      uint8_t r8 = ((c >> 11) & 0x1F) << 3, g8 = ((c >> 5) & 0x3F) << 2, b8 = (c & 0x1F) << 3;
      uint8_t v = (r8 & 0xE0) | ((g8 & 0xE0) >> 3) | (b8 >> 6);
      hex[n++] = hx[v >> 4];
      hex[n++] = hx[v & 15];
    }
    hex[n] = 0;
    Serial.println(hex);
    Serial.flush();
  }
  Serial.println("[shot] END");
}

static void help() {
  Serial.println("[telem] state  - dump detections (the detection oracle)");
  Serial.println("[telem] shot   - screen as RGB332 rows (best-effort)");
  Serial.println("[telem] ver    - firmware version");
#ifdef FUD_BENCH
  Serial.println("[telem] fake <tracker|camera|drone|subghz|hackdev|attack|jam> [key] - inject a test detection");
  Serial.println("[telem] anim [spin|sweep] - trigger the Home idle wheel/radar now (blank = random)");
#endif
}

#ifdef FUD_BENCH
static void fakeInject(const String& a) {
  struct F { const char* n; Cat c; const char* label; const char* name; bool active; char conf; };
  static const F T[] = {
    {"tracker", CAT_TRACKER, "AirTag", "AirTag", false, 'H'},
    {"camera",  CAT_CAMERA,  "Flock ALPR", "Flock Safety", false, 'H'},
    {"drone",   CAT_DRONE,   "Remote ID", "DJI Mini", false, 'H'},
    {"subghz",  CAT_SUBGHZ,  "433 burst", "433.92 MHz", false, 'H'},
    {"hackdev", CAT_HACKDEV, "Flipper", "Flipper Zero", true, 'H'},
    {"attack",  CAT_ATTACK,  "Deauth", "Deauth flood", true, 'H'},
    {"jam",     CAT_ATTACK,  "Jammer", "2.4 GHz jammer", true, 'H'},
  };
  int sp = a.indexOf(' ');
  String what = sp < 0 ? a : a.substring(0, sp);
  uint8_t kb = sp < 0 ? (uint8_t)esp_random() : (uint8_t)a.substring(sp + 1).toInt();
  for (auto& f : T) {
    if (what != f.n) continue;
    Evt e{};
    e.key[0] = 0xFA; e.key[1] = 0x4E; e.key[2] = (uint8_t)f.c; e.key[5] = kb;
    e.cat = f.c;
    snprintf(e.label, sizeof(e.label), "%s", f.label);
    snprintf(e.name, sizeof(e.name), "%s", f.name);
    e.rssi = -40 - (kb % 40);
    e.ssid = false; e.active = f.active; e.conf = f.conf;
    if (evtQ) xQueueSend(evtQ, &e, 0);
    Serial.printf("[telem] fake %s key %02X\n", f.n, kb);
    return;
  }
  Serial.println("[telem] fake what? tracker camera drone subghz hackdev attack jam");
}
#endif

static void poll() {
  String line;
  if (!readLine(line)) return;
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String c = (sp < 0 ? line : line.substring(0, sp));
  String arg = (sp < 0 ? String("") : line.substring(sp + 1));
  c.toLowerCase();
  if (c == "state") dumpState();
  else if (c == "shot") dumpShot();
  else if (c == "ver" || c == "version") Serial.printf("[ver] FUD Firmware %s\n", FUD_VERSION);
  else if (c == "help" || c == "?") help();
#ifdef FUD_BENCH
  else if (c == "fake") { String al = arg; al.toLowerCase(); fakeInject(al); }
  else if (c == "anim") { String al = arg; al.toLowerCase(); homeAnimNow(al=="spin"?1:(al=="sweep"?2:0)); Serial.printf("[telem] anim %s\n", al.length()?al.c_str():"random"); }
#endif
  else Serial.printf("[telem] unknown '%s'. try: state, shot, ver, help\n", c.c_str());
  Serial.printf("[telem] DONE %s\n", c.c_str());
}

}  // namespace telem
