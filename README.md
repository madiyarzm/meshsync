# meshsync

Offline federated learning over a mesh of ESP32 boards, synced with a CRDT over ESP-NOW.

## Layout

```
firmware/        ESP32 sketch (opens in the Arduino IDE); face_app.* = the browser <-> hub protocol
firmware/src/meshstate/   portable library: mergeable slots, deletion, models, sync (C++17, no allocation)
sim/sim.cpp      laptop simulator of the mesh with a faulty radio
web/             face controller page (index.html, app.js, features.js; vendor/ = MediaPipe, Apache-2.0)
web/playground/  visual playground: open index.html (the library + simulator as WebAssembly)
test/            C++ tests for the core, including a golden test against the Python prototype
experiments/     Python prototype and Phase 1 experiments
```

## Flash a board (Arduino IDE)

1. Install the "esp32" board package by Espressif (version 3.x) in Boards Manager.
2. Open `firmware/firmware.ino`, set `BOARD_LETTER` in `config.h` ('A', 'B', ...).
   `APP_MODE` picks the app: `APP_FACE` (default, the face controller page over USB) or
   `APP_DIGITS` (the digit demo).
3. Board: "ESP32 Dev Module", then Upload.
4. Digit demo only: on a phone, join Wi-Fi `Madiyar-Hub-<letter>` and open http://192.168.4.1.

Or with PlatformIO: `pio run -e esp32dev -t upload`.
Send `BENCH` in the serial monitor (115200) to time merges and predictions on the board.

## Laptop: tests and simulator (PlatformIO)

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
on Python 3.10-3.14 (for example `uv tool install platformio --python 3.13`).

```
pio test -e native                      # C++ unit and golden tests
pio run -e native                       # build the simulator (rebuild after pio test)
.pio/build/native/program               # one simulated run (seed 1, 6 hubs)
.pio/build/native/program --seeds 500   # automated test: converge, no resurrection, nothing lost
.pio/build/native/program --capacity 50 # more people than the store holds
.pio/build/native/program --hops 50     # relay timing along a chain of hubs
```

## Face controller page

```
python3 -m http.server 8000 -d web    # then open http://localhost:8000 in Chrome or Edge
```

"Connect hub (USB)" talks to a face-mode board over Web Serial (close the Arduino serial monitor
first). "Use a simulated hub" runs three hubs in the page instead, so no board is needed.
MediaPipe needs WebGL ("Use graphics acceleration" on in Chrome's settings).

## Visual playground

```
python3 -m http.server 8000 -d web    # then open http://localhost:8000/playground/
web/playground/build.sh                          # rebuild after changing the library (needs Emscripten)
```

## Python prototype

```
pip install -r experiments/requirements.txt
python experiments/run_experiments.py                   # accuracy, exactness, size tables
python experiments/run_experiments.py --export-golden   # also rewrite test/test_golden/golden_data.h
```
