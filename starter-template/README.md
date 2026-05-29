# Starter template

A minimal, ready-to-build ESP-IDF project for the BSides Vancouver badge.
The intent is that you can build and flash this in under five minutes,
see the LEDs light up, and then start adding your own behaviour without
having to learn the entire production firmware first.

---

## What's already done for you

- The hardware is brought up: all pins are configured, the FastLED chain
  is initialized, the charlieplex matrix scaffolding works, and the
  GPIO directions for both buttons and sensors are set.
- A short rainbow runs at boot so you know the firmware is alive.
- All the vendored libraries (Arduino-ESP32, FastLED, NFC RFAL,
  ST25R3911B driver) are wired into the CMake build by reusing the
  `components/` folder from the main firmware tree — you don't have to
  copy anything.

## What's *not* done

The main loop is empty. Everything beyond "boot and idle" is up to you:
- Reacting to button presses
- Reading the Hall / lux sensors
- Driving the indicator (charlieplexed) LEDs based on something
- Reading NFC tags
- Sleep / power management
- WiFi / web UI

The main firmware in `../main/` has working examples of every one of
these — copy from there when you get stuck.

---

## Build & flash

```sh
cd starter-template
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

If you're new to ESP-IDF, follow [`../docs/BUILDING.md`](../docs/BUILDING.md)
for the full step-by-step setup.

---

## Layout

```
starter-template/
├── README.md                ← you are here
├── CMakeLists.txt           ← top-level ESP-IDF project file
├── sdkconfig.defaults       ← shared Kconfig with the main firmware
└── main/
    ├── main.cpp             ← edit this — setup(), loop(), helpers
    ├── project_pins.h       ← pin map + constants (same as main firmware)
    └── CMakeLists.txt
```

## Idea menu

Five small projects, ordered by difficulty:

1. **Make BTN1 cycle through colors on RGB[0].** Read `digitalRead(BTN1)`,
   pick a new colour each press, call `FastLED.show()`.
2. **Bouncing pixel.** Walk a single bright pixel back and forth across
   the RGB chain. `delay()` between frames.
3. **Hall sensor flashlight.** When a magnet is present, light *all*
   RGB LEDs white. Remove magnet, all dark. (The badge's Hall sensor is
   a bipolar latch — present one polarity to turn on, the opposite to
   turn off.)
4. **Brightness slider via lux.** Read the lux sensor with `analogRead`
   and feed the value to `FastLED.setBrightness()`. Cover the sensor →
   LEDs dim. Uncover → bright.
5. **Mini Morse beacon.** Pick a string. Translate to dots/dashes. Blink
   them on RGB[0]. Repeat forever. (Hint: dot = 200 ms on; dash =
   600 ms on; symbol gap = 200 ms off; letter gap = 600 ms off; word
   gap = 1400 ms off.)
