# meshsync

**Offline, private, shared calibration on a mesh of ESP32 boards.**

Gesture calibration is stored on small ESP32 boards and shared between them over a direct radio
link (ESP-NOW), with no internet access and no central server. Calibrate at one board and any
board recognizes you. A new user starts from everyone else's calibration. Deleting a user removes
their data from every board, including boards that were offline at the time.


https://github.com/user-attachments/assets/9da8b5ee-485f-42ba-9a1e-0822ac07727a

> **Status: first iteration (v0.1).** It works end to end on real boards. Several parts are not
> measured or built yet; see [Limits](#limits) and [Next](#next).

## How it works

- **On the laptop:** MediaPipe finds the face in each webcam frame and measures 12 features
  (eyebrow height, mouth width, …). Only those numbers, 55–70 bytes per frame, go to the board
  over USB. The video never leaves the laptop.
- **On the board:** calibration is stored as running totals per user and gesture (sample count,
  sum, sum of squares). The board builds the model from those totals and classifies each frame.
- **Between boards:** every second each board broadcasts a fingerprint of its state. When two
  fingerprints differ, the boards exchange only what's missing, and boards in the middle relay it.
- **Exact merging:** because the totals simply add up, combining boards gives exactly the model
  you'd get from all the data in one place. Merge order, lost packets and repeats don't change it
  (the state is a CRDT: per-slot versions, tombstones, and a per-user deletion counter).
- **Deletion:** "delete my data" raises the user's deletion counter. Any board that sees the higher
  counter erases that user's data, including data it recorded while it was offline.

The webcam is a stand-in signal. The part this project is about is underneath: decoder
calibration that is shared, merged exactly, synced offline, and deletable.

## Measured so far

| What | Result |
|---|---|
| Sync between two real boards (calibration saved → other board has it) | 1.3 s |
| Page → board → page round trip (300 predictions) | 18 ms typical, 41 ms worst |
| One prediction on the ESP32 (BENCH) | 0.17 ms gesture model |
| Simulator: 500 random runs, 20% packet loss, partitions, deletions | 500/500 converged, no deleted data came back, nothing deleted by mistake |
| Relay along a line of 6 boards (simulator) | ~1.7 s per hop for a calibration, ~0.9 s for a deletion |
| Digits prototype: merged model vs centralized | identical (90.7% / 90.4%), C++ bit-identical to Python on 450/450 |
| Tests | 29 C++ tests (core, golden, page protocol) |

## Try it without hardware

Everything except the radio runs on a laptop. Requires Chrome or Edge (Web Serial, WebGL).

```
python3 -m http.server 8000 -d web    # then open http://localhost:8000
```

Press **Simulate**: three simulated boards run in the page, compiled from the same C++ that runs on
the ESP32. **How it works** in the page walks through the design. A visual playground of the sync
protocol is at http://localhost:8000/playground/.

## Run on real boards

Hardware: two or more ESP32 dev boards (tested on ESP32-D0WD-V3 with a CP2102 USB chip).

1. Arduino IDE with the Espressif "esp32" package 3.x. Open `firmware/firmware.ino`.
2. In `firmware/config.h`, set `BOARD_LETTER` ('A', 'B', …) for each board. `APP_MODE` is
   `APP_FACE` (this demo) or `APP_DIGITS` (an earlier digit-drawing demo).
3. Board "ESP32 Dev Module", Upload. Power the other boards from any USB charger.
4. Serve `web/` as above, press **Connect**, and pick the board's USB port.

Or with PlatformIO: `pio run -e esp32dev -t upload`.

## Tests and simulator

```
pio test -e native                      # unit, golden and protocol tests
pio run -e native                       # build the simulator
.pio/build/native/program --seeds 500   # convergence + deletion checks over 500 random runs
.pio/build/native/program --hops 50     # relay timing along a chain of boards
```

## Repository

```
firmware/               ESP32 sketch; face_app.* is the page <-> board protocol
firmware/src/meshstate/ the library: mergeable slots, deletion, models, sync (C++17, no allocation)
sim/                    mesh simulator with a faulty radio and partitions
web/                    the page (MediaPipe files in web/vendor/, Apache-2.0)
web/playground/         sync visualizer (library + simulator as WebAssembly)
test/                   C++ tests, including a golden test against the Python prototype
experiments/            Python prototype and first experiments
media/                  demo video
```

## Limits

- Gesture accuracy across people, and how much shared calibration helps a new user, are not
  measured yet. The demo shows it working, not how well.
- Up to 64 calibration slots across the whole mesh (about 30 users). Above that, boards refuse new
  data safely but can't all hold everything.
- Deletions aren't authenticated: any board can issue one. Fine for a demo, not for real use.
- Tested with two physical boards; larger meshes only in the simulator.
- Eye-gaze control is planned, not built.

## Next

- Measure accuracy with and without shared calibration, across several people and laptops.
- Head-pointer cursor and click gestures.
- Timing of the two-core design while syncing.

## License

MIT. MediaPipe files in `web/vendor/` are Apache-2.0.
