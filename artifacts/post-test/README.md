# POST test (StickS3 + RF Pack S3)

Throwaway diagnostic firmware. Serial only, no display, no M5 libs. It
verifies silicon, both I2C buses, and both RF Pack radios on the confirmed
pin map, then takes live passive samples (WiFi scan, 2.4 GHz RPD sweep,
CC1101 RSSI). Not part of FUD firmware. Kept for reference and reruns.

## Run
```
pio run -t upload        # build + flash
pio device monitor -b 115200
```
Native USB port shows as /dev/cu.usbmodem* on this Mac.

## Confirmed on 2026-09-29 (see ../../development-log.txt for full results)
- ESP32-S3 rev 2, 8 MB flash, 8 MB PSRAM working.
- I2C SYS 47/48: ES8311 0x18, BMI270 0x68, PMIC 0x6e.
- CC1101 (CS2/GDO0 3, bus 5/4/6) answers, VERSION 0x14.
- nRF24 (CSN8/CE1, bus 5/4/6) answers, passive 2.4 GHz carriers seen.

Flashing this overwrites Bruce (or FUD). Reflash the real firmware to
return the device to normal.
