# BSides Vancouver Conference Badge — Firmware

[![Project home](https://img.shields.io/badge/repo-maltchev%2Fbsides--vancouver--badge-blue?logo=github)](https://github.com/maltchev/bsides-vancouver-badge)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF v5.4.1](https://img.shields.io/badge/ESP--IDF-v5.4.1-red)](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32c3/index.html)
[![Target ESP32-C3](https://img.shields.io/badge/target-ESP32--C3-purple)](https://www.espressif.com/en/products/socs/esp32-c3)

Firmware for the BSides Vancouver conference badge: an ESP32-C3-based wearable
with an NFC reader, addressable RGB LEDs, charlieplexed indicator LEDs, a
hall-effect sensor, an ambient light sensor, two buttons, and an optional WiFi
access point that hosts a live control panel and a mini Capture-the-Flag
challenge.

This repository contains the source code and documentation needed to build,
flash, and modify the badge firmware. It is the basis for the BSides
Vancouver badge-workshop sessions.

**Repository:** <https://github.com/maltchev/bsides-vancouver-badge>

---

## What the badge does

- Reads NFC tags handed out during the conference. Tags can change the
  badge's team colour, record session attendance, or trigger achievement
  LEDs.
- Lights a row of five WS2812B addressable RGB LEDs and six monochrome
  charlieplexed LEDs to represent your progress.
- Detects a magnet via a bipolar-latching Hall sensor (the badge LED flips
  on with one polarity, off with the other).
- Measures ambient light to enter "in-pocket" light sleep automatically.
- Optionally broadcasts an open WiFi access point on long-press of BTN2 with
  a phone-friendly control panel and a deliberately-vulnerable mini-CTF
  hosted at `http://192.168.4.1/`.
- Persists progress in NVS so power-cycling the badge doesn't reset it.

For a full feature tour, see [`docs/SOFTWARE.md`](docs/SOFTWARE.md).
For the hardware reference, see [`docs/HARDWARE.md`](docs/HARDWARE.md).
For the NFC tag commands, see [`docs/NFC_TAGS.md`](docs/NFC_TAGS.md).
For the **schematic PDF**, see [`docs/BSides_Badge_2025_r2.0_Schematics.PDF`](docs/BSides_Badge_2025_r2.0_Schematics.PDF).

---

## Quick start

1. Install ESP-IDF v5.4.1 — see [`docs/BUILDING.md`](docs/BUILDING.md).
2. Clone this repository:
   ```sh
   git clone https://github.com/maltchev/bsides-vancouver-badge.git
   cd bsides-vancouver-badge
   ```
3. From the project root:
   ```sh
   idf.py set-target esp32c3
   idf.py build
   idf.py -p <PORT> flash monitor
   ```
4. On a fresh badge, the first boot runs a quick component self-test
   (light show, then magnet → cover lux → BTN1 → BTN2). After that it
   drops into normal operation.

If the badge is "stuck" after a software experiment, hold **BTN2** while
plugging USB in — that triggers a hard reset which wipes all persisted state.

---

## Repository layout

```
.
├── README.md                  ← you are here
├── LICENSE
├── CHANGELOG.md
├── CMakeLists.txt             ← top-level ESP-IDF project file (main firmware)
├── sdkconfig.defaults         ← committed Kconfig overrides for the badge
├── version.txt                ← human-readable firmware version string
│
├── main/                      ← main firmware source code
│   ├── main.cpp               ← entry point, main loop, NFC/sensor/LED logic
│   ├── project_pins.h         ← hardware pin map and tunable constants
│   ├── badge_state.h/.cpp     ← NVS-backed persistent state
│   ├── wifi_portal.h/.cpp     ← optional WiFi AP + web UI + CTF
│   └── CMakeLists.txt
│
├── components/                ← vendored third-party libraries
│   ├── FastLED/                       ← RGB LED driver
│   ├── NFC_RFAL/                      ← ST RFAL (NFC abstraction)
│   ├── ST25R3911B/                    ← NFC reader chip driver
│   └── espressif__arduino-esp32/      ← Arduino layer (digitalRead, delay, …)
│
├── starter-template/          ← blank canvas for workshop attendees
│   ├── README.md
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/                          ← minimal setup() + empty loop()
│
├── prebuilt/                  ← ready-to-flash binaries (no IDF needed)
│   ├── README.md
│   └── 2025-production/               ← original 2025 badge firmware
│
└── docs/
    ├── BUILDING.md                       ← dev environment setup, building, flashing
    ├── HARDWARE.md                       ← what's on the board, pinout, schematic notes
    ├── SOFTWARE.md                       ← firmware architecture & boot flow
    ├── NFC_TAGS.md                       ← NFC tag command reference
    ├── WORKSHOP.md                       ← workshop module flow (for facilitators)
    ├── BSides_Badge_2025_r2.0_Schematics.PDF   ← authoritative hardware schematic
    └── slides/
        ├── software-workshop.pptx        ← presenter deck (PowerPoint)
        ├── software-workshop.md          ← same deck, Marp markdown (renders on GitHub)
        └── build_deck.py                 ← regenerates the PPTX
```

---

## Versions

`version.txt` holds the human-readable firmware version (e.g. `v2.4.0`).
This string is printed at boot, surfaced over the JSON status endpoint when
the WiFi portal is active, and used to verify that a freshly-flashed badge is
running the intended build.

See [`CHANGELOG.md`](CHANGELOG.md) for the high-level history.

---

## Workshop attendees

If you're here because of the BSides Vancouver badge workshop:

- The full step-by-step guide is in [`docs/WORKSHOP.md`](docs/WORKSHOP.md).
- You should *not* need to read every source file to participate.
- The "tinker first" recommended path: build, flash, scan an NFC tag,
  toggle the WiFi portal, then dip into the code wherever you got curious.

---

## License

MIT — see [`LICENSE`](LICENSE).
