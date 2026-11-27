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
  "toys","ssid","traffic","tvbg","finder","gps","help","wardrive"
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

#ifdef FUD_BENCH
// Heap low-water log: each time the minimum free heap falls, note when, which radio phase
// (activeRadio) and which screen was up. Last 8 drops, printed by state.
struct HeapDrop { uint32_t up, min; char ph; uint8_t mode; };
static HeapDrop g_drops[8]; static int g_dropN = 0; static uint32_t g_lastMin = 0;
static void heapWatch() {
  uint32_t m = ESP.getMinFreeHeap();
  if (g_lastMin && m >= g_lastMin) return;
  if (g_lastMin && g_lastMin - m < 512) { g_lastMin = m; return; }   // ignore small steps
  g_lastMin = m;
  HeapDrop& d = g_drops[g_dropN % 8]; d.up = millis() / 1000; d.min = m; d.ph = activeRadio; d.mode = (uint8_t)mode; g_dropN++;
}
#endif
static void dumpState() {
  Serial.println("[state] BEGIN");
  Serial.printf("[state] fw %s screen=%s radio=%c muted=%d cc=%d nrf=%d\n",
                FUD_VERSION, MODE_NAMES[mode], activeRadio, muted ? 1 : 0,
                ccPresent ? 1 : 0, nrfPresent ? 1 : 0);
  Serial.printf("[state] display on=%d lock=%d shake=%s bright=%u\n", displayOn ? 1 : 0, screenLock ? 1 : 0, SHAKE_NAMES[shakeIdx], (unsigned)userBright);
  Serial.printf("[state] up=%lus heap=%u min=%u maxblk=%u reset=%d\n",
                (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(), (int)esp_reset_reason());
#ifdef FUD_BENCH
  for (int i = g_dropN > 8 ? g_dropN - 8 : 0; i < g_dropN; i++) { const HeapDrop& d = g_drops[i % 8];
    Serial.printf("[state] heapdrop up=%lus min=%lu radio=%c screen=%s\n", (unsigned long)d.up, (unsigned long)d.min, d.ph, MODE_NAMES[d.mode]); }
#endif
  { char t[24]; clk::stamp(t, sizeof(t)); Serial.printf("[state] clock=%s %s\n", clk::sourceName(), t); }
  if (sdlog::avail) Serial.printf("[state] sdlog on=%d card=%d file=%s written=%lu queued=%d dropped=%lu\n", sdlog::on ? 1 : 0,
                                  sdlog::present() ? 1 : 0, sdlog::path, (unsigned long)sdlog::written, sdlog::queued(), (unsigned long)sdlog::dropped);
  if (wd::avail) Serial.printf("[state] wardrive on=%d buf=%d file=%s wifi=%lu ble=%lu lastmin=%lu written=%lu queued=%d dropped=%lu seen=%lu\n", wd::on ? 1 : 0,
                               wd::buf ? 1 : 0, wd::path, (unsigned long)wd::nWifi, (unsigned long)wd::nBle, (unsigned long)wd::lastMinute(),
                               (unsigned long)wd::written, wd::queued(), (unsigned long)wd::dropped, (unsigned long)wd::seenCount);
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
    // A fixed-landscape board (Cardputer ADV) has no panel read line, so read its frame sprite.
    bool spr = lastFramePcL && pcLReady;
    if (spr) pcL.readRect(0, y, w, 1, row); else M5.Display.readRect(0, y, w, 1, row);
    int n = snprintf(hex, sizeof(hex), "R%d:", y);
    for (int x = 0; x < w; x++) {
      uint16_t c = row[x];
      c = (uint16_t)((c >> 8) | (c << 8));   // readRect (panel or sprite) returns big-endian words; swap to RGB565
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
  Serial.println("[telem] sdtail [w] [n] - last n lines of the SD log (w = wardrive file), printed by the scan task");
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
  else if (c == "gps") Serial.printf("[gps] %s G%d@%lu bytes %lu ok %lu bad %lu status %c q %u sats %d hdop %.1f alt %.0f fix %d %.6f %.6f utc %s %s\n",
      gps::S.enabled ? (gps::S.found ? "found" : "searching") : "none", gps::S.pin, (unsigned long)gps::S.baud,
      (unsigned long)gps::S.bytes, (unsigned long)gps::S.good, (unsigned long)gps::S.bad, gps::S.status, gps::S.quality,
      gps::S.sats, gps::S.hdop, gps::S.altM, (int)gps::fixed(), gps::S.lat, gps::S.lon, gps::S.utc, gps::S.date);
  else if (c == "sdtail") {   // sdtail [n] = detection log, sdtail w [n] = wardrive file, sdtail /fud/x.csv [n]
    static char tp[40]; bool w = arg.startsWith("w"), f = arg.startsWith("/");
    int sp2 = arg.indexOf(' '); String rest = (w || f) ? (sp2 < 0 ? String("") : arg.substring(sp2 + 1)) : arg;
    if (f) { snprintf(tp, sizeof tp, "%s", (sp2 < 0 ? arg : arg.substring(0, sp2)).c_str()); sdlog::tailPath = tp; }
    else if (w) { if (!wd::path[0]) { Serial.println("[telem] sdtail: no wardrive file yet this boot"); Serial.println("[telem] DONE sdtail"); return; } sdlog::tailPath = wd::path; }
    else sdlog::tailPath = nullptr;
    int n = rest.toInt(); sdlog::tailReq = n > 0 ? n : 10;
    Serial.println("[telem] sdtail: prints after the next BLE phase"); }
  else if (c == "sdrm") {   // bench: delete /fud files whose name starts with the prefix (sdrm wigle_20261127_)
    static char pre[32]; snprintf(pre, sizeof pre, "%s", arg.c_str());
    if (strlen(pre) < 6 || strchr(pre, '/')) { Serial.println("[telem] sdrm <name prefix, 6+ chars, no slash>"); }
    else { sdlog::rmPrefix = pre; sdlog::lsReq = true; Serial.println("[telem] sdrm: deletes after the next BLE phase"); } }
  else if (c == "sdls") { sdlog::lsReq = true; Serial.println("[telem] sdls: prints after the next BLE phase"); }
  else if (c == "ver" || c == "version") Serial.printf("[ver] FUD Firmware %s\n", FUD_VERSION);
  else if (c == "help" || c == "?") help();
#ifdef FUD_BENCH
  else if (c == "fake") { String al = arg; al.toLowerCase(); fakeInject(al); }
  else if (c == "go") {   // bench: jump to a screen without pressing keys blind
    String al = arg; al.toLowerCase();
    if (al == "home") goHome();
    else if (al == "status") { mode = MODE_STATUS; }
    else if (al == "zoom") { mode = MODE_ZOOM; }
    else if (al == "settings") { mode = MODE_SETTINGS; }
    else if (al == "toys") { mode = MODE_TOYS; }
    else if (al == "log") { logFilter = -1; listSel = listTop = 0; mode = MODE_LIST; }
    else if (al == "gps") { mode = MODE_GPS; }
    else if (al == "wardrive") { if (wd::avail) { mode = MODE_WARDRIVE; wdAsk = true; } }   // the warning first, like the menu
    else if (al == "waterfall") { openWaterfall(); }
    else if (al == "traffic") { mode = MODE_TRAFFIC; trafPeak = trafNow = 0; }
    else if (al == "ssid" || al == "tvbg" || al == "finder") {   // open by name, never by cursor position
      const char* want = al == "ssid" ? "SSID Scan" : al == "tvbg" ? "TV-B-Gone" : "Finder";
      for (int i = 0; i < TOYS_N; i++) if (!strcmp(TOYS_ITEMS[i], want)) { toysSel = i; mode = MODE_TOYS; actFrontHold(); }
    }
    else { Serial.println("[telem] go home|status|zoom|settings|toys|log|gps|waterfall|traffic|ssid|tvbg|finder|wardrive"); return; }
    uiDirty = true; wake(INTERACT_TIMEOUT);
  }
  else if (c == "key") {  // bench: the same path as a real Cardputer key press
    String al = arg; al.toLowerCase();
    static const struct { const char* n; uint8_t k; } K[] = {{"up", KC_UP}, {"down", KC_DOWN}, {"left", KC_LEFT},
      {"right", KC_RIGHT}, {"enter", KC_ENTER}, {"esc", KC_ESC}, {"q", KC_Q}, {"m", KC_M}, {"a", KC_A}, {"l", KC_L}, {"h", KC_H}};
    bool ok = false;
    for (auto& e : K) if (al == e.n) { ok = true;   // same lock rules as keysPoll
      if (screenLock) { if (lockDebounced()) setLock(false); } else if (e.k == KC_L) setLock(true); else { wake(holdForMode()); keyAct(e.k); } }
    if (!ok) { Serial.println("[telem] key up|down|left|right|enter|esc|q|m|a|l|h"); return; }
  }
  else if (c == "anim") { String al = arg; al.toLowerCase(); homeAnimNow(al=="spin"?1:(al=="sweep"?2:0)); Serial.printf("[telem] anim %s\n", al.length()?al.c_str():"random"); }
#endif
  else Serial.printf("[telem] unknown '%s'. try: state, shot, ver, help\n", c.c_str());
  Serial.printf("[telem] DONE %s\n", c.c_str());
}

}  // namespace telem
