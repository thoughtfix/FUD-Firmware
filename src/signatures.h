// FUD detection signatures. Include AFTER enum Cat is defined.
//
// These are factual radio identifiers (Bluetooth SIG company IDs, GATT service
// UUIDs, vendor SSID prefixes), not copyrightable expression. Signature facts
// were cross-checked against the OUI-SPY project (colonelpanichacks) and the
// SquachWatch-CYD catalog; the values are public registry data. Confidence:
// 'H' high (vendor-registered), 'M' medium, 'L' low (generic/shared).
#pragma once
#include <stdint.h>

// ---- BLE: simple single-signature matches (service UUID and/or company ID) ----
// service 0xFFFF = don't care; company 0xFFFF = don't care.
struct BleSig { uint16_t service; uint16_t company; Cat cat; const char* label; char conf; };
static const BleSig BLE_SIGS[] = {
  { 0xFEED, 0xFFFF, CAT_TRACKER, "Tile",      'H' },
  { 0xFEEC, 0xFFFF, CAT_TRACKER, "Tile",      'H' },
  { 0xFD5A, 0xFFFF, CAT_TRACKER, "SmartTag",  'H' },  // Samsung SmartTag
  { 0xFFFF, 0x09C8, CAT_CAMERA,  "Flock",     'H' },  // Flock/XUNTONG mfg company ID
  { 0xFFFF, 0x034D, CAT_CAMERA,  "Axon",      'H' },  // TASER Intl (Axon body cams)
  { 0xFFFA, 0xFFFF, CAT_DRONE,   "RemoteID",  'M' },  // ASTM F3411 drone Remote ID
  // From FUD-Deck's 2026-11-13 backport (SquachWatch-CYD, Talking Sasquach, GPL-3.0, commits
  // 2026-10-14 to 2026-11-03). Service UUIDs match the advertised list OR service data.
  { 0xFD84, 0xFFFF, CAT_TRACKER, "Tile",      'H' },  // Tile's third SIG-assigned UUID
  { 0xFCB2, 0xFFFF, CAT_TRACKER, "Tracker",   'H' },  // DULT: any maker's tag away from its owner
  { 0xFE33, 0xFFFF, CAT_TRACKER, "Chipolo",   'H' },
  { 0xFE65, 0xFFFF, CAT_TRACKER, "Chipolo",   'H' },
  { 0xFFFF, 0x08C3, CAT_TRACKER, "Chipolo",   'M' },
  { 0xFC81, 0xFFFF, CAT_CAMERA,  "Axon",      'H' },  // Axon's SIG-assigned UUIDs
  { 0xFE6B, 0xFFFF, CAT_CAMERA,  "Axon",      'H' },
  { 0xFE6C, 0xFFFF, CAT_CAMERA,  "Axon",      'H' },
  { 0x3100, 0xFFFF, CAT_CAMERA,  "Raven",     'M' },  // Flock Raven gunshot sensor
  { 0x3200, 0xFFFF, CAT_CAMERA,  "Raven",     'M' },
  { 0x3300, 0xFFFF, CAT_CAMERA,  "Raven",     'M' },
  { 0x3400, 0xFFFF, CAT_CAMERA,  "Raven",     'M' },
  { 0x3500, 0xFFFF, CAT_CAMERA,  "Raven",     'M' },
  // Camera glasses by company ID. Meta's IDs are on Quest headsets too: a Meta radio, not proof
  // of a camera, so M. The 0xFD5F composite in main.cpp is the specific one.
  { 0xFFFF, 0x01AB, CAT_CAMERA,  "Meta",      'M' },
  { 0xFFFF, 0x0D53, CAT_CAMERA,  "Luxottica", 'M' },  // Ray-Ban's maker
  { 0xFFFF, 0x03C2, CAT_CAMERA,  "Snap",      'M' },  // Snap Spectacles
  { 0xFFFF, 0x060C, CAT_CAMERA,  "Vuzix",     'M' },
  { 0xFE45, 0xFFFF, CAT_CAMERA,  "Snap",      'M' },
  { 0xFEB7, 0xFFFF, CAT_CAMERA,  "Meta",      'M' },  // also Quest headsets: cameras, Meta hardware
  { 0xFEB8, 0xFFFF, CAT_CAMERA,  "Meta",      'M' },
  // Card skimmers: Bluetooth serial-port profile (SPP) in a BLE advert (SquachWatch-CYD).
  // Classic-only skimmers (most HC-05s) need BT Classic inquiry, which we do not run.
  { 0x1101, 0xFFFF, CAT_HACKDEV, "Skimmer",   'M' },
};
static const int BLE_SIGS_N = sizeof(BLE_SIGS)/sizeof(BLE_SIGS[0]);

// ---- Special composites / payload checks (handled in code, not the table) ----
// Meta/Ray-Ban glasses: Luxottica CID 0x0D53 AND Meta svc UUID 0xFD5F in the
//   SAME advert (single 0xFD5F false-positives on phones running Meta apps).
// Real Ray-Ban Meta glasses send 0x01AB (Meta) with 0xFD5F (SquachWatch-CYD, two captures).
#define META_CID   0x0D53
#define META_CID2  0x01AB
#define META_SVC   0xFD5F
// Flipper Zero advertises one of three fixed service UUIDs (one per case colour), so a renamed
// Flipper is still caught, and so is one whose name only arrives in a scan response (we scan
// passively). Flipper's company ID 0x0E29 is NOT used: only the BUSY Bar desk sign sends it.
#define FLIPPER_SVC0  0x3081
#define FLIPPER_SVC_N 3
// Google Find Hub (FMDN): 0xFEAA service data whose frame type is 0x40 or 0x41. Eddystone shop
// beacons share 0xFEAA, so the UUID alone is not a tracker.
#define FMDN_SVC   0xFEAA
// Flock Safety's own 128-bit BLE service (POTENTIAL_IMPROVEMENTS.md, SquachWatch-CYD).
#define FLOCK_UUID128 "e8ccbb38-9532-46a8-9fe5-1814df172e6f"
// Apple Find My / AirTag: company 0x004C with offline-finding type byte 0x12.
#define APPLE_CID  0x004C
#define FINDMY_TYPE 0x12

// ---- WiFi: SSID-prefix signatures ----
struct SsidSig { const char* prefix; Cat cat; const char* label; bool active; };
static const SsidSig SSID_SIGS[] = {
  { "AB2-",       CAT_CAMERA,  "Axon",   false },  // Axon body cam SSIDs
  { "AB3-",       CAT_CAMERA,  "Axon",   false },
  { "AB4-",       CAT_CAMERA,  "Axon",   false },
  { "AXON-",      CAT_CAMERA,  "Axon",   false },
  { "Pineapple_", CAT_HACKDEV, "PineAP", true  },  // WiFi Pineapple (hacker device)
  // Flock setup and camera networks (SquachWatch-CYD, Flock-You #43). All SSID prefixes are
  // matched case-insensitively. A bare "Flock" SSID is matched exactly in main.cpp.
  { "flock-",       CAT_CAMERA, "Flock", false },
  { "Flock Camera", CAT_CAMERA, "Flock", false },
};
static const int SSID_SIGS_N = sizeof(SSID_SIGS)/sizeof(SSID_SIGS[0]);

// ---- WiFi OUI signatures (first 3 MAC bytes) ----
// One signal, not the last word: a match means "candidate", not proof. OUIs
// verified against IEEE registry lookups (via oui-response.txt research).
struct OuiSig { uint8_t oui[3]; Cat cat; const char* label; char conf; };
static const OuiSig OUI_SIGS[] = {
  {{0x18,0xB4,0x30}, CAT_CAMERA, "Nest",  'H'},   // Nest Labs cameras
  {{0x64,0x16,0x66}, CAT_CAMERA, "Nest",  'H'},
  {{0x48,0x62,0x64}, CAT_CAMERA, "Arlo",  'H'},   // Arlo cameras
  {{0xA4,0x11,0x62}, CAT_CAMERA, "Arlo",  'H'},
  {{0xFC,0x9C,0x98}, CAT_CAMERA, "Arlo",  'H'},
  {{0x98,0xED,0x5C}, CAT_CAMERA, "Tesla", 'M'},   // Tesla (Sentry cams; experimental)
  // Meta Platforms hardware (glasses / Quest / Portal). We say "a Meta product
  // is near you"; the glasses get the precise BLE composite elsewhere.
  {{0x48,0x05,0x60}, CAT_CAMERA, "Meta",  'M'},
  {{0x50,0x99,0x03}, CAT_CAMERA, "Meta",  'M'},
  {{0x78,0xC4,0xFA}, CAT_CAMERA, "Meta",  'M'},
  {{0x80,0xF3,0xEF}, CAT_CAMERA, "Meta",  'M'},
  {{0x84,0x57,0xF7}, CAT_CAMERA, "Meta",  'M'},
  {{0x88,0x25,0x08}, CAT_CAMERA, "Meta",  'M'},
  {{0x94,0xF9,0x29}, CAT_CAMERA, "Meta",  'M'},
  {{0xB4,0x17,0xA8}, CAT_CAMERA, "Meta",  'M'},
  {{0xC0,0xDD,0x8A}, CAT_CAMERA, "Meta",  'M'},
  {{0xCC,0xA1,0x74}, CAT_CAMERA, "Meta",  'M'},
  {{0xD0,0xB3,0xC2}, CAT_CAMERA, "Meta",  'M'},
  {{0xD4,0xD6,0x59}, CAT_CAMERA, "Meta",  'M'},
  {{0xF4,0x4E,0x35}, CAT_CAMERA, "Meta",  'M'},
  {{0x04,0xA8,0x5A}, CAT_DRONE,  "DJI",   'H'},   // SZ DJI (drones)
  {{0x0C,0x9A,0xE6}, CAT_DRONE,  "DJI",   'H'},
  {{0x34,0xD2,0x62}, CAT_DRONE,  "DJI",   'H'},
  {{0x48,0x1C,0xB9}, CAT_DRONE,  "DJI",   'H'},
  {{0x4C,0x43,0xF6}, CAT_DRONE,  "DJI",   'H'},
  {{0x58,0xB8,0x58}, CAT_DRONE,  "DJI",   'H'},
  {{0x60,0x60,0x1F}, CAT_DRONE,  "DJI",   'H'},
  {{0x88,0x29,0x85}, CAT_DRONE,  "DJI",   'H'},
  {{0x8C,0x58,0x23}, CAT_DRONE,  "DJI",   'H'},
  {{0xE4,0x7A,0x2C}, CAT_DRONE,  "DJI",   'H'},
  // ---- Surveillance vendors ----
  // This block (the camera / ALPR / Ring / Flock / Axon rows and their H/M
  // confidence grades) is a selected subset DERIVED FROM the kOuiTable in
  // SquachWatch-CYD (https://github.com/skizzophrenic/SquachWatch-CYD), which is
  // licensed GPL-3.0. The OUI values and the per-row confidence calls are their
  // compiled research; we reformatted into our own struct, merged Ring/ALPR into
  // CAT_CAMERA, and dropped their ~40 Low-confidence rows (generic Espressif/
  // SiLabs/QCA silicon blocks that match any ESP32, including this board). Credit
  // and thanks to SquachWatch-CYD. See POTENTIAL_IMPROVEMENTS.md and README.
  {{0x00,0x25,0xDF}, CAT_CAMERA, "Axon",     'H'},   // Axon / TASER
  {{0xB4,0x1E,0x52}, CAT_CAMERA, "Flock",    'H'},   // Flock Safety (MA-L block)
  {{0x00,0x04,0x7D}, CAT_CAMERA, "ALPR",     'H'},   // Motorola Solutions plate readers
  {{0x00,0x18,0x85}, CAT_CAMERA, "ALPR",     'H'},
  {{0x00,0x1F,0x92}, CAT_CAMERA, "ALPR",     'H'},
  {{0x4C,0xCC,0x34}, CAT_CAMERA, "ALPR",     'H'},
  {{0xB8,0xE2,0x8C}, CAT_CAMERA, "ALPR",     'H'},
  {{0x00,0xBF,0x15}, CAT_CAMERA, "ALPR",     'H'},   // Genetec
  {{0x0C,0xBF,0x15}, CAT_CAMERA, "ALPR",     'H'},
  {{0x2C,0xAA,0x8E}, CAT_CAMERA, "Wyze",     'H'},
  {{0xD0,0x3F,0x27}, CAT_CAMERA, "Wyze",     'H'},
  {{0x7C,0x78,0xB2}, CAT_CAMERA, "Wyze",     'H'},
  {{0x34,0xD2,0x70}, CAT_CAMERA, "Amazon",   'M'},   // Amazon (Ring/Blink shared block)
  {{0xF0,0x27,0x2D}, CAT_CAMERA, "Amazon",   'M'},
  {{0xC0,0x56,0xE3}, CAT_CAMERA, "Hikvision",'H'},
  {{0x44,0x19,0xB6}, CAT_CAMERA, "Hikvision",'H'},
  {{0x28,0x57,0xBE}, CAT_CAMERA, "Hikvision",'H'},
  {{0xE0,0xA7,0x00}, CAT_CAMERA, "Verkada",  'H'},
  {{0x70,0x1A,0xD5}, CAT_CAMERA, "Avigilon", 'H'},
  {{0x00,0x40,0x8C}, CAT_CAMERA, "Axis",     'H'},
  {{0xB8,0xA4,0x4F}, CAT_CAMERA, "Axis",     'H'},
  {{0xFC,0x65,0xDE}, CAT_CAMERA, "Ring",     'M'},
  {{0x68,0x37,0xE9}, CAT_CAMERA, "Ring",     'M'},
  {{0xAC,0x9F,0xC3}, CAT_CAMERA, "Ring",     'H'},
  {{0x18,0x7F,0x88}, CAT_CAMERA, "Ring",     'H'},
  {{0x34,0x3E,0xA4}, CAT_CAMERA, "Ring",     'H'},
  {{0x54,0xE0,0x19}, CAT_CAMERA, "Ring",     'H'},
  {{0x5C,0x47,0x5E}, CAT_CAMERA, "Ring",     'H'},
  {{0x64,0x9A,0x63}, CAT_CAMERA, "Ring",     'H'},
  {{0x90,0x48,0x6C}, CAT_CAMERA, "Ring",     'H'},
  {{0x9C,0x76,0x13}, CAT_CAMERA, "Ring",     'H'},
  {{0xCC,0x3B,0xFB}, CAT_CAMERA, "Ring",     'H'},
  {{0xC4,0xDB,0xAD}, CAT_CAMERA, "Ring",     'H'},
  {{0x24,0x2B,0xD6}, CAT_CAMERA, "Ring",     'H'},
  {{0x00,0xB4,0x63}, CAT_CAMERA, "Ring",     'H'},
  {{0x50,0xE4,0x67}, CAT_CAMERA, "Ring",     'H'},
  // From FUD-Deck's 2026-11-13 backport (SquachWatch-CYD, registry-checked there).
  // WatchGuard VIDEO (Motorola), not the firewall maker WatchGuard Technologies.
  {{0x00,0x1D,0x96}, CAT_CAMERA, "WatchGuard",'M'},   // body cams
  {{0x00,0x23,0xBD}, CAT_CAMERA, "DigitalAlly",'M'},  // body cams
  {{0xD4,0x11,0xD6}, CAT_CAMERA, "ShotSpotter",'H'},  // gunshot sensors
  {{0xD4,0x2D,0xC5}, CAT_CAMERA, "i-PRO",    'H'},    // Panasonic security and body cams
  {{0x3C,0xA0,0x70}, CAT_CAMERA, "Blink",    'M'},    // Blink by Amazon's own blocks
  {{0x70,0xAD,0x43}, CAT_CAMERA, "Blink",    'M'},
  {{0x74,0x13,0x48}, CAT_CAMERA, "Blink",    'M'},
  {{0x74,0xAB,0x93}, CAT_CAMERA, "Blink",    'M'},
  {{0xC8,0x19,0xD8}, CAT_CAMERA, "Blink",    'M'},
  {{0xF0,0x74,0xC1}, CAT_CAMERA, "Blink",    'M'},
  {{0x98,0x59,0x49}, CAT_CAMERA, "Luxottica",'M'},    // Ray-Ban / Oakley Meta
  {{0x38,0x1D,0x14}, CAT_DRONE,  "Skydio",   'H'},
  {{0x90,0x03,0xB7}, CAT_DRONE,  "Parrot",   'M'},
  {{0x90,0x3A,0xE6}, CAT_DRONE,  "Parrot",   'M'},
  {{0xA0,0x14,0x3D}, CAT_DRONE,  "Parrot",   'M'},
};
static const int OUI_SIGS_N = sizeof(OUI_SIGS)/sizeof(OUI_SIGS[0]);

// Remaining ideas (see POTENTIAL_IMPROVEMENTS.md): the low-confidence generic
// Flock ESP32 OUIs with a name/GATT gate so they do not flag every ESP32;
// Bluetooth Classic skimmer prefixes; the Flock BLE GATT UUID; full Remote ID
// (ASTM F3411) decode; and a persistence engine that tracks a rotating-MAC
// device by service UUID + mfg structure + RSSI trajectory + advert interval.
