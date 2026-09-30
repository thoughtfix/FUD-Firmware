# Potential improvements

Things FUD could gain, mostly features other detectors in this class have that
we do not yet. The broader WiFi OUI tables that used to be on this list are now
implemented (Flock registered block, ALPR Motorola/Genetec, Ring, Wyze, Amazon,
Hikvision, Verkada, Avigilon, Axis, Axon). What remains:

## Detection coverage

- **Bluetooth Classic skimmers.** HC-05/06/03, RN42, BT04-A card skimmers show
  up as BT Classic with SPP UUID 0x1101 and a few known OUIs. We scan BLE only
  (NimBLE), so Classic inquiry scanning is the missing piece, and it is awkward
  to run alongside BLE on this chip. Worth it: skimmers are a real street threat.

- **Flock BLE GATT UUID.** Flock cameras expose a custom 128-bit accessory
  service (e8ccbb38-9532-46a8-9fe5-1814df172e6f). Matching it is a strong Flock
  signal independent of the WiFi OUI. Our BLE table is 16-bit only today, so this
  needs a 128-bit match path in the BLE classifier.

- **Generic Flock ESP32 OUIs, gated.** Flock also ships on stock Espressif,
  SiLabs, and QCA silicon blocks. Matching those raw would flag every ESP32 in
  range (including this board), so they were left out. They only become useful
  behind a second gate: a Flock-ish advertised name or the GATT UUID above.

- **ASTM F3411 Remote ID decode.** We flag a drone's Remote ID broadcast as
  present. Decoding the payload would give aircraft position, altitude, serial,
  and the operator's location. This is a real parser, not a signature.

- **Raven gunshot sensor.** Service UUIDs 0x3100 to 0x3500. One table row each.

- **Pwnagotchi beacon.** Detect its advertised beacon payload, not just the
  Pineapple and `pwned` SSIDs we already catch.

## Behavior

- **Rotating-MAC persistence.** Trackers (AirTags especially) rotate their BLE
  address every few minutes, which splits one device into many log entries and
  breaks a Finder lock. A persistence engine could hold one identity across the
  rotation using service UUID + manufacturer-data shape + RSSI trajectory +
  advertising interval. This would also make the Finder stick to an AirTag.

## Separate effort

- **Alert tuning (deauth and 2.4 jam false positives).** The 2.4 GHz jam trip
  point (40 of 126 channels) is too low for a dense RF environment, and the
  deauth detector fires on a flat frame count regardless of reason code. Both
  want real-world tuning (raise and require persistence for jam; lean on
  broadcast + reason code for deauth). Tracked for its own pass, not here.

## Deliberately out of scope

Not gaps, choices. FUD stays a passive worn canary: no device-to-device social
or mesh, no identity broadcast, no on-board storage or SD logging, no GPS or
wardriving, no over-the-air / network-join updates, and no leveling or
collectible gamification. The one sanctioned transmit is TV-B-Gone.
