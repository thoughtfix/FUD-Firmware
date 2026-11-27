// Board profiles. One firmware, chosen at boot from M5Unified's board detection.
// Nothing else in the firmware names a pin number for these parts.
//
//   StickS3 + Pingequa RF Pack S3: radios on their own pins (5/4/6).
//   Cardputer ADV + Pingequa Hydra RF: radios share the SD bus (40/39/14), SD CS 12. The Hydra
//   isolates whichever radio is not selected, so the rule is simply: never two CS lines low.
//
// The radios use FSPI (SPI2) on every board. M5GFX drives the display on SPI3 (HSPI) on both,
// so the radios and the display never share one SPI peripheral.
//
// Rotation: M5GFX gives the ADV panel an offset of 1, so its landscape is rotation 0. On the
// StickS3, 0 is portrait and 1 is landscape. The ADV screen is fixed landscape, so both of its
// values are landscape (its own landscape screens replace the portrait ones later).
#pragma once
#include <M5Unified.h>

struct Board {
  const char* name;
  bool known;              // false = no RF probe and no IR (pins unknown)
  bool cardputer;
  int8_t sck, miso, mosi;  // RF backpack SPI
  int8_t ccCs, ccGdo0;     // CC1101
  int8_t nrfCsn, nrfCe;    // nRF24
  int8_t sdCs;             // -1 = no SD slot. Held high whenever the radios use the bus.
  int8_t irTx;             // -1 = no IR LED
  int8_t groveA, groveB;   // Grove pins for a UART GPS (listen only), -1 = none
  uint8_t rotPortrait, rotLandscape;
};

static const Board BOARD_STICKS3   = {"StickS3",      true,  false,  5,  4,  6,  2, 3, 8, 1, -1, 46, -1, -1, 0, 1};
static const Board BOARD_CARDPUTER = {"CardputerADV", true,  true,  40, 39, 14, 13, 5, 6, 4, 12, 44,  1,  2, 0, 0};
static const Board BOARD_UNKNOWN   = {"unknown",      false, false, -1, -1, -1, -1,-1,-1,-1, -1, -1, -1, -1, 0, 1};

// Call after M5.begin().
static inline const Board& boardDetect() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5StickS3:      return BOARD_STICKS3;
    case m5::board_t::board_M5CardputerADV: return BOARD_CARDPUTER;
    default:                                return BOARD_UNKNOWN;
  }
}
