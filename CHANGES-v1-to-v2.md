# What's different in v2

This is a workshop-focused refactor of the BSides 2025 badge firmware. **Behaviour is identical to v1** — every NFC command, every LED, every sensor reaction works the same. Only the code shape changes.

The original v1 lives in `Workshop/BSides-Badge-Source/` if you want to compare side-by-side.

---

## File layout

| File | v1 | v2 |
|---|---|---|
| `main/main.cpp` | 1340 lines | ~700 lines |
| `main/project_pins.h` | — | **new** — pin and hardware-count map |
| `main/badge_state.h` | — | **new** — NVS save/restore interface |
| `main/badge_state.cpp` | — | **new** — NVS save/restore implementation |
| `main/CMakeLists.txt` | 1 source file | 2 source files, include dir set |

Total source size is roughly the same (~900 vs 1340 lines), but split across three logical files instead of one.

---

## NVS keys are unchanged

Every NVS key name (`RGB1_red`, `presLED1`, `presTrack1`, `initialize`, etc.) is identical to v1. A badge previously running v1 will load its saved state correctly under v2 with no migration needed.

---

## Refactors applied

### 1. Pins moved to `project_pins.h`

Every `#define BTN1 9`, `#define X1 21`, `#define NUM_LEDS 5`, etc. that was sprinkled across the top of `main.cpp` now lives in a single header with grouped comments. If the hardware changes, that's the only file you edit.

### 2. NVS persistence wrapped in a `BadgeState` namespace

v1 called `badgeState.begin("badge", false)` and `badgeState.end()` around every individual getter/setter — that pattern appeared 9 times.

v2 has named operations (`BadgeState::saveTeamColours()`, `BadgeState::savePresentationProgress()`, `BadgeState::restoreAll()`, etc.) that own the open/close lifecycle themselves. Call sites in `main.cpp` no longer mention `begin`/`end`.

### 3. Six identical `LED1()`–`LED6()` functions replaced by one table

v1 had 6 nearly-identical 7-line functions, each setting up the three charlieplex pins one way:

```c
static void LED1(void) {
  pinMode(X1, OUTPUT);
  pinMode(X2, OUTPUT);
  pinMode(X3, INPUT);
  digitalWrite(X1, HIGH);
  digitalWrite(X2, LOW);
}
static void LED2(void) { /* nearly the same */ }
// ... LED3, LED4, LED5, LED6
```

v2 replaces them with a single 6×3 lookup table (`CLED_TABLE`) and one `lightCharlieplexLed(idx)` function. Adding a 7th LED would be a one-row addition, not a new function.

### 4. Six identical team-colour cases replaced by a lookup loop

v1 had this six times, once per colour:

```c
case g:
  ESP_LOGW(TAG, "Green Team");
  RGB_LEDS_Save_State[Team_Colour1] = CRGB::Green;
  if (NUM_LEDS == 5) {
    RGB_LEDS_Save_State[Team_Colour2] = CRGB::Green;
  }
  saveTeamState();
  break;
```

v2 has one `TEAM_COLOURS[]` table and a single loop that handles all six.

### 5. Eight identical presentation-attendance cases replaced by a loop

v1's `case t:` was a 44-line if/else-if chain with eight near-identical branches. v2 has a `PRESENTATION_CODES[8]` array and one `for` loop.

### 6. 24-line presentation-counter switch replaced by a lookup table

v1's `updatePresentationCounter()` was a switch over 7 cases setting three LED bits each. v2 looks up a `{l1, l2, l3}` row from `PRES_PATTERNS[]`.

### 7. NFC state machine became a proper enum

v1: `#define NOTINIT 0`, `#define START_DISCOVERY 1`, etc. — magic ints that share the same namespace as everything else. v2: `enum BadgeFsm { BADGE_NOTINIT, BADGE_START_DISCOVERY, ... }`.

### 8. Cryptic single-char aliases removed

v1 had `const char r = 'R';` then `case r:`. v2 uses `case 'R':` directly. (For the team-colour table, the literal `'R'` is in the `cmd` field, which is its actual purpose.)

### 9. Bugs fixed

- **Variable shadowing in `startupLights()`.** v1's inner loop reused `int i` from the outer loop. Worked by accident. v2 names them `led_idx` and `cycle`.
- **`case 'X'` (wipe) used to log "Wiping all saved data" *before* checking the wipe code matched.** So the log lied if the code was wrong. v2 logs after the check passes, and logs an "Invalid badge-wipe code" message on mismatch.
- **`case 'V'` (Malware Village) had no log when the code didn't match.** v2 logs "Invalid Malware Village code" for symmetry with the other commands.

### 10. Magic numbers named

`5 * 1000000`, `120`, `1000`, `100`, `2500`, `10` for sensor thresholds and timeouts are now in `project_pins.h` as named constants (`HALL_HOLD_DURATION_MS`, `LUX_DARK_THRESHOLD`, `INACTIVITY_TIMEOUT_S`, etc.).

### 11. Dead comments removed

- `// #define MAX_HEX_STR 4` and `// #define MAX_HEX_STR_LENGTH 128` (commented out, unused)
- `//#define BTN2 GPIO_NUM_2` (moved into a meaningful block in `project_pins.h`)
- `// RGB 3 = ` (with nothing assigned — now documented properly in the slot-aliases section)

### 12. GPIO log spam silenced

Without this, the serial monitor is unreadable. Every `pinMode(...)` in `CLEDDisplay()` triggers an ESP-IDF `gpio: GPIO[NN]| InputEn: ... OutputEn: ...` INFO line, and `CLEDDisplay()` runs every loop iteration with 6 mode changes per pass — the `Badge:` logs you actually care about scroll off-screen before you can read them.

The fix is one line at the top of `setup()`:

```c
esp_log_level_set("gpio", ESP_LOG_WARN);
```

This was also applied to v1 retroactively, so both source trees produce a readable log.

---

## What did NOT change

- **NFC code paths** — RFAL initialization, the discovery state machine, NDEF decoding, the RTD_TEXT handler — all kept verbatim. These are correct and not the source of any readability problems.
- **Variant guards** — `if (NUM_LEDS == 5)` and `if (BTN2 == GPIO_NUM_2)` are preserved as a teaching moment for handling hardware variants in a single codebase.
- **Arduino-as-component** — still used as in v1.
- **`Preferences` API** — v2's `BadgeState` still uses `Preferences` under the hood; the production firmware uses raw `nvs_*` calls but porting to those is a larger change deferred to a future session.
- **Component libraries** — `FastLED/`, `NFC_RFAL/`, `ST25R3911B/`, `espressif__arduino-esp32/` are unchanged.

---

## Building v2

Same as v1. From an ESP-IDF 5.4.1 shell:

```
cd Workshop/BSides-Badge-Source-v2
idf.py set-target esp32c3
idf.py build
idf.py -p COM<x> flash monitor
```

If a build error pops up that wasn't there with v1, that's a bug in v2 worth reporting. Falling back to v1 is always an option:

```
cd Workshop/BSides-Badge-Source
idf.py build
```

---

*v2 prepared from v1 = commit `cd70c41` of `sbuchana/BSides-Vancouver-2025-Badge` with production tweaks applied. Pin map, behaviour, and NVS schema all unchanged.*
