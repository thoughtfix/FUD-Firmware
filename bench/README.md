# Bench

Test code for the bench. None of it ships: no release or CI build reads this folder
(CI builds `env:sticks3`, whose sources are `src/` only). Each test is its own PlatformIO
environment that builds one file from here, using the board profiles in `src/board.h`.

| Env | File | What it does |
|---|---|---|
| `cardputer-hwtest` | `hwtest_cardputer.cpp` | Cardputer ADV + Hydra RF bring-up: both radios, SD, GPS, keyboard, IMU, battery, speaker, display. IR fires only on the serial `ir` command. Halts on any other board. |
| `cardputer-hwtest-qspi` | same | Quad memory fallback for the PSRAM-less FN8. |

## tools

Python 3 with pyserial. Each script prints its usage at the top of the file.

- `serread.py`: read a port and send commands
- `duo.py`: two units at once, timed commands to each
- `stab.py`: uptime and heap sampling for stability runs
- `shots.py`, `shot2png.py`: screenshots via the bench `go` and `shot` commands
