# FUD roadmap

Where FUD is going. This is a plan, not a promise, and it will move around.
For the detailed catalog of smaller ideas, see
[POTENTIAL_IMPROVEMENTS.md](POTENTIAL_IMPROVEMENTS.md).

## Status: v0.40b, first public beta

FUD works. It scans, classifies, grades confidence, alerts, and the toys run.
It has been written and bench-tested enough that it looks right and behaves on
real gear. It has NOT yet been tested against the full range of simulated
hostile attacks, and there is not yet a written QA path. So this is a beta.

Download it, use it, experiment with it. Just know that it is early, that some
alerts still false-positive (see near-term below), and that detections are a
first line of defense, not proof. Forks and contributions are welcome under
GPL-3.0. Keep it passive: no hostile transmit (the one sanctioned TX is
TV-B-Gone).

## The long-term goal

Firmware for a variety of devices, aimed squarely at less-experienced security
researchers who want to learn and experiment without accidentally breaking laws.
It is unsettling how many people finish a Pwnagotchi and take it for a walk
without realizing they are probably breaking three different laws on the way
(deauthentication, attempted association, interception). FUD is deliberately a
receiver and a canary, so you can learn the airwaves without transmitting into
them. The idea generalizes to other hardware over time.

## Near term (before a wider "stable" push)

1. **QA and a real test harness.** Build the rig and scripts to generate the
   signals FUD claims to catch, and confirm it catches them repeatably. Using a
   Flipper Zero, an M5Cardputer ADV, and a couple of CYD boards to stand in for
   deauth floods, evil twins, 2.4 GHz jamming, tracker adverts, Remote ID, and
   sub-GHz traffic. The goal is "tested," not "appears to work."
2. **AirTag spam filtering.** Find My churn floods the tracker list and the
   Finder. Roll up and dedupe it, and start on holding one identity across a
   rotating MAC (see POTENTIAL_IMPROVEMENTS.md, rotating-MAC persistence).
3. **Menu and UX cleanup.** Early field use turned up a few spots where the
   menus are unintuitive. Smooth those.
4. **Alert tuning.** The deauth and 2.4 GHz jam detectors false-positive in a
   busy RF environment. Raise the jam threshold and require it to persist across
   sweeps; lean on broadcast target and reason code for deauth. This is its own
   focused pass.

## Planned features

- **Max Scan mode.** An opt-in mode that drops the time-sliced radio rotation
  and scans every radio as hard and as fast as possible, trading battery life
  and heat for coverage. Off by default, behind a clear warning. This is the
  opposite of the normal canary duty cycle and is meant for a deliberate sweep,
  not all-day wear.

## Backlog

Detailed in [POTENTIAL_IMPROVEMENTS.md](POTENTIAL_IMPROVEMENTS.md): Bluetooth
Classic skimmers, ASTM F3411 Remote ID decode, the Flock BLE GATT signature,
gated generic-ESP32 OUIs, Raven, the Pwnagotchi beacon, and the rotating-MAC
persistence engine. Further out, ports to other devices (Cardputer, CYD, and
friends) as the project generalizes.

## Not on the roadmap, by design

FUD stays a passive worn canary. No offensive or hostile transmit, no device
mesh or social broadcast, no on-board storage or GPS or wardriving, no leveling
or collectible gamification. These are choices, not gaps.

## Helping out

Issues and pull requests are welcome. The best places to start are the near-term
list above and the backlog in POTENTIAL_IMPROVEMENTS.md. If you are adding a
detector, bring the public signature facts (service UUID, company ID, OUI) and a
confidence grade, and keep it passive.
