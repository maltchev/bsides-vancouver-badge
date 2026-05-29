# Workshop Guide

A facilitator's walkthrough for the BSides Vancouver badge workshop.
This document is for the people running the session. Attendees should
read the project README and follow along.

---

## Goals

By the end of the session, every attendee should be able to:

1. Build the firmware from source on their own laptop.
2. Flash it to their badge.
3. Modify at least one user-visible behaviour (an LED colour, a sleep
   timeout, a new web endpoint) and re-flash.
4. Understand the role each component plays in the firmware.

The session is intentionally hands-on. Lecturing time is minimised.

---

## Prerequisites for attendees

Sent ahead in the registration email:

- A laptop with USB-A or USB-C (a USB-A → USB-A cable is provided with
  the badge if needed).
- ~10 GB free disk for ESP-IDF.
- A free Python 3.10+ installation.
- Recommended: VS Code with the official "ESP-IDF" extension.
- Optional: an Android phone with the "NFC TagWriter" app for writing
  custom tags. iOS works too (NFC Tools).

---

## Suggested module order

The session is built as five short modules of ~20 minutes each. Skip
or compress freely depending on attendees' background.

### Module 1 — Tour the badge (10 min)

Hand badges out. Power-on each one. Walk through:

- The five RGB LEDs and what they mean.
- The six charlieplexed indicator LEDs.
- The two buttons.
- The NFC antenna location (top of the board).
- The Hall sensor location.
- The Lux sensor location.

Reference: [`HARDWARE.md`](HARDWARE.md).

### Module 2 — Build & flash (20 min)

Walk the room through:

- Installing ESP-IDF v5.4.1.
- Cloning the project.
- `idf.py set-target esp32c3 && idf.py build && idf.py -p <PORT> flash monitor`.

Reference: [`BUILDING.md`](BUILDING.md).

Pre-built binaries are available in the workshop materials package as a
fallback for attendees who can't install IDF in time.

### Module 3 — NFC tags (20 min)

Each attendee gets a small pack of pre-programmed tags:

- 6 team-colour tags (R / G / B / Y / P / O).
- 1 custom-colour tag (showing the `C` payload format).
- A "session attendance" tag for the workshop itself (counts toward
  the master unlock).
- A "wipe" tag for resetting the badge mid-experiment.

Have them scan each one with BTN1 and watch the LEDs / serial log.
Then have them write a *new* tag with NFC TagWriter — pick a new
custom colour `Crrrgggbbb` and program a blank tag.

Reference: [`NFC_TAGS.md`](NFC_TAGS.md).

### Module 4 — Modify the firmware (40 min)

This is the bulk of the session. Pick one of:

- **Easy** — change `BADGE_BRIGHTNESS_DEFAULT` in `project_pins.h`,
  rebuild, observe.
- **Medium** — add a new NFC command. E.g. `H` for "halt all LEDs and
  flash white once." Add the `case 'H'` in `ndefReadTag()` in
  `main.cpp`, write a new tag, scan it.
- **Hard** — add a new `/api/celebrate` endpoint to `wifi_portal.cpp`
  that triggers the same LED celebration the CTF does, and add a
  button to the control panel HTML.

Have a printed copy of each task on the table so attendees can pick
their own pace.

### Module 5 — WiFi portal + mini-CTF (20 min)

End on a high note. Walk through:

1. Long-press BTN2 → portal comes up.
2. Connect a phone to the open AP.
3. Show the live control panel (everyone change their LED colour from
   their phone simultaneously — fun group moment).
4. Open the CTF challenge. Give a couple of minutes for people to try
   solving it on their own, then walk through the cookie-tampering
   solution if needed.

The CTF flag is `BSIDES{cookies_are_not_credentials}`. Solving it
triggers a celebratory LED flash on the attendee's own badge.

---

## Things that will go wrong, and what to say

These are the ones that consistently come up:

### "It won't flash — `Failed to connect to ESP32-C3`"

Almost always one of:
- A serial monitor is open in another window holding the port (close
  PuTTY/screen/the previous `idf.py monitor`).
- The badge has been through a sleep cycle and `esptool`'s auto-reset
  isn't working. Fix: unplug and re-plug USB, then try again.
- If still failing: hold BTN1 (= GPIO 9 boot strap) while plugging
  USB in.

### "My HALL_LED is stuck on / off and the magnet does nothing"

Expected. The Hall sensor is a bipolar latch — present one polarity
to flip it on, the *opposite* polarity to flip it off. Have them flip
their magnet over.

### "The badge keeps going to sleep"

It's probably in a dim spot and the lux-pocket-sleep is triggering.
Move it under a lamp, or bump `LUX_POCKET_SLEEP_MS` in
`project_pins.h`.

### "I want to wipe everything and start over"

Hold BTN2 while plugging USB in. Red flash, release button — badge is
fully reset.

### "It crashes when I try to flash"

Usually means an attendee has accidentally enabled
`CONFIG_ESP_BROWNOUT_DET=n` in sdkconfig from an earlier diagnostic
attempt. Have them delete `sdkconfig` (NOT `sdkconfig.defaults`) and
`idf.py reconfigure`.

---

## After the workshop

Encourage attendees to keep their badges and keep tinkering. Useful
follow-on directions:

- Replace the WiFi captive portal's HTML with something they author
  from scratch.
- Add a CO₂ or temperature sensor over the existing SPI pads.
- Implement a different game loop entirely (e.g. a step counter using
  the lux sensor as a crude motion proxy).
- Submit a pull request with an improvement!
