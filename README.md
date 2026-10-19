# construct

Offline federated learning over a mesh of ESP32 boards, synced with a CRDT over ESP-NOW.

## Build and test (PlatformIO)

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
running on Python 3.10-3.14 (for example `uv tool install platformio --python 3.13`).

```
pio test -e native            # C++ unit tests on the laptop
pio run -e native             # build the simulator
.pio/build/native/program     # run the simulator
pio run -e esp32dev           # build the ESP32 firmware
pio run -e esp32dev -t upload # flash a board over USB
```

## Python prototype

```
pip install -r experiments/requirements.txt
python experiments/run_experiments.py
python -m pytest experiments
python experiments/export_golden.py   # regenerate test/test_golden/golden_data.h
```
