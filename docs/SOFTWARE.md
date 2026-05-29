# Software Architecture

A guided tour of how the firmware is laid out. The intent is that you
can sit down with the source for the first time and have a mental
model of where everything lives.

The firmware is written in C++ on top of **ESP-IDF v5.4.1**, with the
[**Arduino-as-component**](https://github.com/espressif/arduino-esp32)
layer for `digitalRead`, `analogRead`, `delay`, and the FastLED library.

---

## File layout

All project source lives in `main/`:

| File                      | Responsibility |
|---------------------------|----------------|
| `main.cpp`                | Entry point, hardware setup, main loop, NFC handlers, sensor logic |
| `project_pins.h`          | Hardware pin map + tunable constants. **The only file you need to edit if the PCB changes.** |
| `badge_state.h/.cpp`      | NVS-backed persistent state — restore on boot, save on change |
| `wifi_portal.h/.cpp`      | Optional WiFi access point + HTTP control panel + CTF challenge |
| `CMakeLists.txt`          | ESP-IDF component registration for `main/` |

Third-party components live in `components/`:

| Component                    | What it is |
|------------------------------|------------|
| `espressif__arduino-esp32`   | Arduino-style APIs on top of ESP-IDF |
| `FastLED`                    | RGB LED driver (uses RMT5 backend on ESP32-C3) |
| `NFC_RFAL`                   | ST's RFAL (Radio Frequency Abstraction Layer) |
| `ST25R3911B`                 | Low-level driver for the NFC reader IC |

---

## Boot flow

```
   app_main()
     ↓
   initArduino()           ← brings up the Arduino layer (delay/digitalRead/…)
     ↓
   setup()
     ├─ log CPU freq + reset reason  (always — useful for diagnostics)
     ├─ silence GPIO INFO chatter    (keeps the console readable)
     ├─ if BTN2 is held at boot     → wipeAll() + flash red + wait for release
     ├─ pinMode for every peripheral
     ├─ start FastLED chain
     ├─ startupLights()              ← brief light show
     ├─ BadgeState::restoreAll()     ← reload persisted state from NVS
     ├─ checkAndApplyMasterUnlock()  ← lights RGB[3] if every challenge done
     ├─ componentCheck()             ← only on first boot ever
     └─ rfal_nfc.rfalNfcInitialize() + set TX power
       ↓
   for (;;) loop() ↓
     loop():
       1. Refresh awake-indicator LED on RGB[0]
       2. Drain CTF flag (LED celebration if portal CTF was just solved)
       3. checkHallEffectSensor()    ← sample HALL, update HALL_LED, run hold puzzle
       4. checkLuxSensor()           ← sample LUX, update LUX_LED, run cover puzzle
       5. CLEDDisplay()              ← refresh one charlieplex pin slot
       6. checkButtons()             ← BTN1 = scan, BTN2 = activity / long-press
       7. Sleep gating: pocket sleep, then inactivity sleep
       8. RFAL state machine: idle → discovery → tag-active → back to idle
       9. rfalNfcWorker()            ← service NFC interrupts
```

The main loop runs at full speed; no `delay()` calls in the steady-state
path. Charlieplex refresh handles its own ~1 ms-per-LED delay.

---

## State storage (NVS)

Persisted under namespace `"badge"`. Keys are kept compatible with the
original v1 firmware so an upgraded badge keeps its state.

| Key            | Type | Holds                                                  |
|----------------|------|--------------------------------------------------------|
| `initialize`   | bool | true once `componentCheck()` has been completed        |
| `presCnt`      | uint | count of presentations attended                        |
| `presLED1..3`  | bool | each of the 3 "attendance" charlieplex LED states      |
| `presTrack1..8`| bool | per-session "we already counted this one" tracker      |
| `malwareLED`   | bool | Malware Village charlieplex LED state                  |
| `hallLED`      | bool | reserved (HALL_LED is now driven from live sensor)     |
| `luxLED`      | bool | LUX_LED state                                          |
| `RGB1..5_red/green/blue` | uint | per-channel colour values for each RGB slot |
| `brightness`   | uint | global FastLED brightness (0..255)                     |

All saves are write-through (open namespace, write, close — commits on
close). So a power loss between two save operations can leave you
out-of-sync between fields, but not corrupt within a single field.

---

## Hardware abstractions worth knowing

### Hall sensor (`checkHallEffectSensor()`)

The MGH201A1T3 is a **bipolar latching** Hall switch:

- One magnetic polarity pulls the line LOW → `HALL_LED` on.
- The opposite polarity releases the line HIGH → `HALL_LED` off.
- Removing the magnet **does nothing** — state is held.

The firmware adds a small (200 ms) software debounce so brief noise from
nearby fields (laptop hinges, speakers) doesn't blip the LED. There is
also a "hold for 2.5 s" puzzle that picks a pseudo-random colour for the
HALL_RGB LED when the magnet is held continuously.

### Light sensor (`checkLuxSensor()`)

The ALS-PT19 returns an analog voltage proportional to incident light.
The firmware:
- Lights `LUX_LED` whenever the analog reading is above
  `LUX_LIGHT_THRESHOLD`.
- Treats below `LUX_DARK_THRESHOLD` (well-covered sensor) as "in
  pocket" — sustained darkness for `LUX_POCKET_SLEEP_MS` triggers
  light sleep.
- Has its own "hold for 2.5 s while dark" puzzle that paints LUX_RGB.

### NFC (`demoNdef()` and `ndefReadTag()`)

Sequence on a BTN1 press:

1. `state = BADGE_START_DISCOVERY` — RFAL state machine starts
   polling for NFC-V tags.
2. RFAL fires `rfalNfcIsDevActivated()` when a tag enters the field.
3. We read the NDEF message, decode the first record, and dispatch on
   its first byte (see [`NFC_TAGS.md`](NFC_TAGS.md)).
4. Block until the tag leaves the field (to avoid double-counting),
   then return to idle.

### Sleep

Two paths into light sleep:

- **Inactivity** — no buttons / NFC / puzzle triggers for
  `INACTIVITY_TIMEOUT_S` seconds.
- **Pocket** — lux sensor reports dark continuously for
  `LUX_POCKET_SLEEP_MS` ms.

Wake sources:

- **BTN2** (GPIO 2 LOW) — RTC-capable, always works.
- **LUX** (GPIO 1 HIGH) — coming back into ambient light.

After every wake the firmware **re-initialises the NFC reader**
because the ST25R3911B's internal protocol state drifts during the
SPI silence of light sleep. Without this re-init, NFC tag scans
silently fail after the first sleep cycle.

### WiFi portal

Off at boot; brought up by a **2-second long-press of BTN2**. Hosts:

- `GET /` — single-page live control panel (sensors + LED colour
  pickers + brightness slider).
- `GET /api/state` — JSON snapshot, refreshed by the page every 500 ms.
- `POST /api/color`, `POST /api/brightness` — live LED control.
- `GET /ctf`, `/ctf/login`, `/ctf/dashboard` — the captive-portal
  mini-CTF (cookie-tampering challenge).
- Standard captive-portal probe paths (`/generate_204`,
  `/hotspot-detect.html`, `/ncsi.txt`, etc.) → 302 to `/`.

While the portal is active, sleep is suppressed and the awake-indicator
LED switches from green to blue. Another 2-second long-press of BTN2
takes it back down.

---

## Conventions used in the source

- All hardware constants and pin numbers live in `project_pins.h`. Source
  files never hard-code GPIO numbers or counts.
- The single ESP-IDF log tag for our code is `Badge`. The WiFi portal
  uses `WifiPortal`. Component drivers use their own tags.
- Log levels:
  - `ESP_LOGW(TAG, ...)` for one-shot interesting events (boot, NFC
    init, sleep, wake, CTF flag, …).
  - `ESP_LOGI(TAG, ...)` for short-lived activity (NFC handler progress,
    discovery transitions).
  - `ESP_LOGD(TAG, ...)` for per-loop diagnostics — silent at the
    default INFO log level.
- Global state owned by `main.cpp` is referenced from companion files
  via `extern`, not exposed in a header. Cross-file globals get a `g_`
  prefix (none currently; we removed all of them in v2.4.0).
- `static` for file-local helpers; namespace for the persistent-state
  API.

---

## Extending it

A few examples of how the codebase is meant to be modified:

- **Add a new NFC command.** Add a `case` to the switch in
  `ndefReadTag()` in `main.cpp`. Use the helpers in `BadgeState::` if
  you need to persist anything.
- **Add a new web endpoint.** Edit `wifi_portal.cpp`: add a handler
  function and register it inside `register_handlers()`.
- **Add a new sensor.** Add a `#define` to `project_pins.h`, add a
  `check<Sensor>()` function in `main.cpp`, and call it from `loop()`.
- **Change the badge's idle behaviour.** Most of the relevant numbers
  (sleep timeout, debounce window, brightness, …) are in
  `project_pins.h` — no code changes required.
