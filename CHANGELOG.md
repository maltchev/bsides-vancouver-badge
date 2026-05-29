# Changelog

Notable changes to the BSides Vancouver Badge firmware.

The version string lives in [`version.txt`](version.txt) and is printed at
boot and surfaced over the WiFi portal's status endpoint.

## v2.4.1

### Control panel improvements
- Live web panel now shows **all six charlieplexed indicator LEDs**
  (Presentation 1–3, Malware Village, Hall LED, Lux LED) with their
  PCB designators (D3, D4, D5, D7, D8, D9).
- New **Game progress** card displays the presentation attendance counter,
  Malware Village state, and the master-unlock status with a per-session
  progress strip.
- RGB-LED labels updated to reflect their functional role (HALL_RGB / Awake,
  Master Unlock, etc.) instead of just `RGB_1`/`RGB_2`/etc.
- `/api/state` JSON is restructured: sensors and game-progress fields are
  grouped under nested objects (`sensors{}`, `progress{}`).

## v2.4.0

First public release of the refactored firmware.

### Features
- Full NFC tag-command set: team colour presets (R/G/B/Y/P/O), custom RGB
  (`Crrrgggbbb`), session-attendance tags (T), Malware Village tag (V),
  brightness control (`Lddd`), badge-wipe tag (X).
- Master-unlock indicator on RGB[3] when every attendance challenge is
  complete (lit in BSides orange).
- Hall sensor LED follows the sensor's latched state directly (one polarity
  on, opposite polarity off).
- Lux sensor controls an "in-pocket" light sleep — badge enters light sleep
  after 60 s of darkness, wakes on returning light or BTN2 press.
- Inactivity sleep after 60 s with no activity.
- BTN2 hold-at-startup performs a hard reset (wipes all persisted state).
- Optional WiFi access point with:
  - A single-page live control panel (`/`) showing real-time sensor state
    and offering colour pickers + brightness slider.
  - A captive-portal mini Capture-the-Flag challenge (`/ctf`) demonstrating
    a cookie-tampering bug. Solving it triggers a celebratory LED flash on
    the badge.
  - Toggled on/off with a 2-second long-press of BTN2.
- All persisted state survives reflash (NVS keys are stable).

### Behaviour notes
- The Hall sensor is a **bipolar latching** part (MGH201A1T3) — present one
  polarity to light HALL_LED, the opposite polarity to clear it. The state
  is held even when the magnet is removed.
- WiFi draws too much current to run on coin cells. The portal automatically
  suppresses light sleep while active and is intended for use on USB power.
- The earlier SOS-Morse hall-sensor puzzle has been removed because it is
  fundamentally incompatible with a latching sensor.
