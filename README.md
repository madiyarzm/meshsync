# construct

Offline federated learning over a mesh of ESP32 boards, synced with a CRDT over ESP-NOW.

## Build and test (PlatformIO)

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
running on Python 3.10-3.14 (for example `uv tool install platformio --python 3.13`).

```
pio test -e native                      # C++ unit, golden, and sync tests on the laptop
pio run -e native                       # build the simulator
.pio/build/native/program               # one simulated run (seed 1, 6 hubs)
.pio/build/native/program 42 8          # seed 42, 8 hubs
.pio/build/native/program --seeds 200   # many runs, prints a summary
pio run -e esp32dev                     # build the ESP32 firmware
```

## Flash a board

Set `BOARD_LETTER` at the top of `src/config.h` ('A', 'B', ...), then:

```
pio run -e esp32dev -t upload
pio device monitor
```

Join the Wi-Fi network `construct-<letter>` and open http://192.168.4.1.

## Python prototype

```
pip install -r experiments/requirements.txt
python experiments/run_experiments.py
python -m pytest experiments
python experiments/export_golden.py   # regenerate test/test_golden/golden_data.h
python experiments/mock_hub.py        # the hub's web page on http://localhost:8000, no board needed
```
