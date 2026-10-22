# construct

Offline federated learning over a mesh of ESP32 boards, synced with a CRDT over ESP-NOW.

## Layout

```
firmware/        ESP32 sketch (opens in the Arduino IDE)
firmware/src/meshstate/   portable library: mergeable slots, deletion, models, sync (C++17, no allocation)
sim/sim.cpp      laptop simulator of the mesh with a faulty radio
test/            C++ tests for the core, including a golden test against the Python prototype
experiments/     Python prototype and Phase 1 experiments
```

## Flash a board (Arduino IDE)

1. Install the "esp32" board package by Espressif (version 3.x) in Boards Manager.
2. Open `firmware/firmware.ino`, set `BOARD_LETTER` in `config.h` ('A', 'B', ...).
3. Board: "ESP32 Dev Module", then Upload.
4. On a phone, join Wi-Fi `Madiyar-Hub-<letter>` and open http://192.168.4.1.

Or with PlatformIO: `pio run -e esp32dev -t upload`.
Send `BENCH` in the serial monitor (115200) to time merges and predictions on the board.

## Laptop: tests and simulator (PlatformIO)

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
on Python 3.10-3.14 (for example `uv tool install platformio --python 3.13`).

```
pio test -e native                      # C++ unit and golden tests
pio run -e native                       # build the simulator (rebuild after pio test)
.pio/build/native/program               # one simulated run (seed 1, 6 hubs)
.pio/build/native/program --seeds 100   # automated sync test: all runs must converge
```

## Python prototype

```
pip install -r experiments/requirements.txt
python experiments/run_experiments.py                   # accuracy, exactness, size tables
python experiments/run_experiments.py --export-golden   # also rewrite test/test_golden/golden_data.h
```
