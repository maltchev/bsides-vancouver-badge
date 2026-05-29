// =============================================================================
//  project_pins.h — BSides Vancouver 2025 Badge
// =============================================================================
//
//  Centralised hardware pin map and per-board configuration.
//
//  All GPIO numbers and LED/sensor counts live in this single header so that
//  the rest of the firmware never has to hard-code a pin or a hardware
//  constant. If the badge hardware changes, this is the only file you need
//  to edit.
//
//  Most pin choices were dictated by hardware constraints, not free choice:
//
//   - The ST25R3911B NFC controller uses the standard SPI pins exposed by
//     the Arduino-ESP32 core (MOSI / MISO / SCK / SS) — no relocation needed.
//
//   - BTN2 must be on an RTC-capable GPIO (0..5) so it can wake the ESP32-C3
//     from deep sleep. The prototype put BTN2 on GPIO 11 (not RTC-capable);
//     the production board moved it to GPIO 2 by removing the microphone.
//
//   - The three charlieplex pins (X1/X2/X3) must each support tri-state
//     output (input / output-HIGH / output-LOW). All standard GPIOs do.
//
// =============================================================================
#pragma once

#include "driver/gpio.h"

// -----------------------------------------------------------------------------
// NFC (ST25R3911B over SPI)
// -----------------------------------------------------------------------------
// SPI bus pins follow the Arduino-ESP32 board defaults; only IRQ and CS are
// badge-specific.
#define IRQ                  3        // ST25R3911B IRQ line
#define IRQ_PIN              IRQ      // alias for code clarity
#define CS_PIN               SS       // SPI chip-select (Arduino board default)

// -----------------------------------------------------------------------------
// RGB LEDs (WS2812B-style, driven via FastLED + RMT5)
// -----------------------------------------------------------------------------
#define RGB_LED_PIN          8        // single GPIO drives the addressable chain

// Number of RGB LEDs on the badge.
//   - Prototype board: 3 LEDs
//   - Production badge: 5 LEDs (HALL_RGB, Team_Colour1, LUX_RGB, [reserved], Team_Colour2)
//
// The firmware contains `if (NUM_LEDS == 5) { ... }` guards in several places
// so the same codebase can target both variants.
#define NUM_LEDS             5

// -----------------------------------------------------------------------------
// Charlieplexed monochrome LEDs
// -----------------------------------------------------------------------------
// Three pins drive 6 LEDs through tri-state switching (see CLEDDisplay / LED1..LED6).
#define X1                   21
#define X2                   10
#define X3                   20

#define NUM_CLEDS            6        // 6 monochrome LEDs on a 3-pin charlieplex matrix
#define CLED_Delay           1        // milliseconds each LED is held lit per refresh

// -----------------------------------------------------------------------------
// Sensors
// -----------------------------------------------------------------------------
#define HALL                 0        // hall-effect sensor (digital, active-LOW)
#define LUX                  1        // ambient-light sensor (analog)
// #define MIC               2        // removed for production — pin repurposed for BTN2

// -----------------------------------------------------------------------------
// Buttons
// -----------------------------------------------------------------------------
#define BTN1                 9        // NFC-read trigger

// BTN2 is the wake-from-deep-sleep button. ESP32-C3 only supports deep-sleep
// wake on RTC GPIOs (0..5), so this pin must come from that range. The
// production badge uses GPIO 2 (freed by removing the microphone).
//
// The codebase keeps the prototype line commented out; uncomment it if you
// are rebuilding for the pre-production board.
#define BTN2                 GPIO_NUM_2     // Production
// #define BTN2              GPIO_NUM_11    // Prototype (kept for historical reference)

// -----------------------------------------------------------------------------
// Sensor & timing thresholds
// -----------------------------------------------------------------------------
#define INACTIVITY_TIMEOUT_S    60        // seconds of idle before light sleep
#define HALL_HOLD_DURATION_MS   2500      // hold magnet this long for the puzzle trigger
#define LUX_HOLD_DURATION_MS    2500      // cover lux this long for the puzzle trigger
#define LUX_DARK_THRESHOLD      120       // analog reading below this = covered
#define LUX_LIGHT_THRESHOLD     1000      // analog reading above this = lit

// Hall-effect software debounce. Filters stray magnetic fields from laptops,
// speakers, hinges etc. The HALL_LED indicator and the 2.5-second hold puzzle
// only register a magnet if the sensor reads ACTIVE continuously for this
// many milliseconds. SOS Morse detection uses raw input (its own debouncer).
#define HALL_DEBOUNCE_MS        200

// "In-pocket" sleep: if the lux sensor reads dark continuously for this many
// milliseconds, the badge enters light sleep. Either a button press OR the
// lux sensor returning to bright wakes it up. Inspired by the natural
// gesture of stuffing the badge in your pocket.
//
// Tuned to 60 s so that ordinary use in a dimly-lit room (conference floor,
// evening party, lecture hall) doesn't keep putting the badge to sleep.
// 3000 ms (3 s) is more aggressive and only appropriate for a very bright
// environment where "dark = definitely in a pocket".
#define LUX_POCKET_SLEEP_MS     60000

// -----------------------------------------------------------------------------
// Debug switches
// -----------------------------------------------------------------------------
// Set to 1 to disable both inactivity sleep AND lux-pocket sleep. Useful when
// monitoring over USB Serial/JTAG, because light sleep tears down the USB
// peripheral and the host loses the COM port (no clean way to resume the same
// CDC session on wake). Set back to 0 for normal "in pocket" power saving.
#define BADGE_DEBUG_NO_SLEEP    0

// Set to 1 to drop CPU clock to 80 MHz and skip NFC initialization at boot.
// This lets the badge run from two CR2032 coin cells without brownout-resetting
// in a loop — the ST25R3911B's analog front-end inrush exceeds what the cells
// can supply. Leave at 0 for the normal workshop configuration (USB power,
// full NFC enabled, 160 MHz). Set to 1 only for battery-mode diagnostics.
#define BADGE_LOW_POWER         0

// -----------------------------------------------------------------------------
// LED brightness presets
// -----------------------------------------------------------------------------
// FastLED brightness scale: 0 (off) ... 255 (full). The five WS2812Bs draw
// about 60 mA each at full white; we deliberately stay low so the badge is
// comfortable to look at and friendly to power.
#define BADGE_BRIGHTNESS_BOOT       30      // brief flash during BTN2 hard-reset
#define BADGE_BRIGHTNESS_BOOT_LP    8       // same, but in BADGE_LOW_POWER mode
#define BADGE_BRIGHTNESS_DEFAULT    10      // normal operating brightness

// -----------------------------------------------------------------------------
// Timing constants
// -----------------------------------------------------------------------------
#define BTN_DEBOUNCE_MS         50          // ms ignored between press edges
#define BTN2_LONGPRESS_MS       2000        // hold BTN2 this long → toggle WiFi
#define NFC_TAG_REMOVE_POLL_MS  130         // polling cadence while waiting for tag removal
#define NFC_POST_DEACTIVATE_MS  500         // settle delay after deactivating NFC
#define CTF_PULSE_MS            150         // LED flash duration during CTF celebration
#define CTF_PULSE_COUNT         6           // number of on/off pulses (3 visible flashes)
