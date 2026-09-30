// USER HARD-IGNORE LIST  (compile-time)
//
// Put YOUR OWN devices here so FUD never alerts on them. Handy for testing:
// hide your own Flipper, phone, home WiFi, trackers, etc. These are baked into
// the firmware and always ignored, on top of the runtime Ignore list you build
// on the device. Leave a list empty to ignore nothing of that kind.
#pragma once
#include <stdint.h>

// Your own WiFi network names (exact match). Silences evil-twin / PineAP hits
// for these SSIDs, e.g. a badly configured home AP.
static const char* USER_IGNORE_SSIDS[] = {
  // "MyHomeWiFi",
  // "MyHomeWiFi_5G",
};

// Your own device MAC / BSSID addresses (BLE or WiFi), 6 bytes each.
static const uint8_t USER_IGNORE_MACS[][6] = {
  // { 0xAA,0xBB,0xCC,0xDD,0xEE,0xFF },
};

// Your own BLE advertised names, matched as a substring. Hides e.g. your own
// named Flipper Zero so it does not read as a threat during tests.
static const char* USER_IGNORE_BLE_NAMES[] = {
  // "Flipper MyName",
};

// sizes (auto)
static const int USER_IGNORE_SSIDS_N     = sizeof(USER_IGNORE_SSIDS)/sizeof(USER_IGNORE_SSIDS[0]);
static const int USER_IGNORE_MACS_N      = sizeof(USER_IGNORE_MACS)/sizeof(USER_IGNORE_MACS[0]);
static const int USER_IGNORE_BLE_NAMES_N = sizeof(USER_IGNORE_BLE_NAMES)/sizeof(USER_IGNORE_BLE_NAMES[0]);
