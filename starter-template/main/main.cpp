// =============================================================================
//  main.cpp — BSides Vancouver Badge — Starter Template
// =============================================================================
//
//  This is a deliberately-minimal starting point. The hardware is set up
//  for you, a quick light show confirms the LEDs work, and then the main
//  loop is empty. From here you can build whatever you want.
//
//  What's already wired up:
//    - FastLED for the 5 addressable RGB LEDs (RGB_LEDS[])
//    - Charlieplex matrix scaffolding (state buffers + refresh helper)
//    - GPIO directions for both buttons and both sensors
//    - The ESP-IDF logging macros (ESP_LOGW, ESP_LOGI, etc.)
//
//  What's NOT wired up (left for you):
//    - NFC tag reading
//    - Light / hall sensor reactions
//    - Sleep modes
//    - WiFi / web UI
//    - NVS persistence
//
//  Look at the main firmware in `../main/` for working examples of all of
//  these once you're ready to graduate from the starter template.
//
// =============================================================================

#include "Arduino.h"
#include "FastLED.h"
#include "esp_log.h"

#include "project_pins.h"

static const char *TAG = "Starter";

// -----------------------------------------------------------------------------
// LED state
// -----------------------------------------------------------------------------
CRGB RGB_LEDS[NUM_LEDS];                  // addressable RGB chain
uint8_t CLED_State[NUM_CLEDS] = {0};      // 1 = lit, 0 = dark, per charlieplex LED

// -----------------------------------------------------------------------------
// Charlieplex helpers (three-pin tri-state matrix driving six LEDs)
// -----------------------------------------------------------------------------
enum { TRI = 0, HIGH_OUT = 1, LOW_OUT = 2 };

// Per-LED pin states.  Row index = charlieplex LED index (0..5).
//   TRI       — pin is high-impedance (input)
//   HIGH_OUT  — pin is driven HIGH
//   LOW_OUT   — pin is driven LOW
//
// Each row has exactly one HIGH and one LOW; the third is tri-stated so only
// one LED in the matrix is lit at a time.
static const uint8_t CLED_TABLE[NUM_CLEDS][3] = {
    { HIGH_OUT, LOW_OUT, TRI      },   // LED0
    { LOW_OUT,  HIGH_OUT, TRI     },   // LED1
    { TRI,      HIGH_OUT, LOW_OUT },   // LED2
    { TRI,      LOW_OUT,  HIGH_OUT},   // LED3
    { LOW_OUT,  TRI,      HIGH_OUT},   // LED4
    { HIGH_OUT, TRI,      LOW_OUT },   // LED5
};

static void lightCharlieplexLed(uint8_t idx) {
    static const int pins[3] = {X1, X2, X3};
    for (int i = 0; i < 3; i++) {
        switch (CLED_TABLE[idx][i]) {
            case TRI:       pinMode(pins[i], INPUT);                         break;
            case HIGH_OUT:  pinMode(pins[i], OUTPUT); digitalWrite(pins[i], HIGH); break;
            case LOW_OUT:   pinMode(pins[i], OUTPUT); digitalWrite(pins[i], LOW);  break;
        }
    }
}

// Call this once per loop iteration to refresh the charlieplex matrix.
// Lights each LED whose CLED_State[idx] == 1 in turn, then parks the pins.
static void CLEDDisplay(void) {
    for (int i = 0; i < NUM_CLEDS; i++) {
        if (CLED_State[i]) lightCharlieplexLed(i);
        delay(CLED_Delay);
    }
    pinMode(X1, INPUT);
    pinMode(X2, INPUT);
    pinMode(X3, INPUT);
}

// -----------------------------------------------------------------------------
// Boot-time eye candy
// -----------------------------------------------------------------------------
static void startupLights(void) {
    static uint8_t hue = 0;
    FastLED.setBrightness(BADGE_BRIGHTNESS_DEFAULT);
    for (int i = 0; i < NUM_LEDS; i++) {
        RGB_LEDS[i] = CHSV(hue, 255, 255);
        FastLED.show();
        delay(150);
        hue += 40;
    }
    for (int i = 0; i < NUM_LEDS; i++) RGB_LEDS[i] = CRGB::Black;
    FastLED.show();
}

// =============================================================================
// setup() — runs once at boot
// =============================================================================
void setup() {
    ESP_LOGW(TAG, "BSides Badge — Starter Template starting up.");
    ESP_LOGW(TAG, "CPU frequency: %li MHz", getCpuFrequencyMhz());

    // --- RGB chain --------------------------------------------------------
    pinMode(RGB_LED_PIN, OUTPUT);
    FastLED.addLeds<NEOPIXEL, RGB_LED_PIN>(RGB_LEDS, NUM_LEDS);
    FastLED.setBrightness(BADGE_BRIGHTNESS_DEFAULT);

    // --- Charlieplex pins -------------------------------------------------
    pinMode(X1, OUTPUT);
    pinMode(X2, OUTPUT);
    pinMode(X3, OUTPUT);

    // --- Buttons + sensors ------------------------------------------------
    pinMode(BTN1, INPUT);
    pinMode(BTN2, INPUT);
    pinMode(HALL, INPUT_PULLUP);   // open-drain output → needs a pull-up
    pinMode(LUX,  INPUT);

    startupLights();
    ESP_LOGW(TAG, "Boot complete. Hello, world!");

    // ─────────────────────────────────────────────────────────────────────
    //   TODO: your one-shot initialization goes here.
    // ─────────────────────────────────────────────────────────────────────
}

// =============================================================================
// loop() — runs repeatedly forever
// =============================================================================
void loop() {
    // Keep the charlieplex matrix refreshing so any LED you set in
    // CLED_State[] actually shows up. Cost is a few ms per loop iteration.
    CLEDDisplay();

    // ─────────────────────────────────────────────────────────────────────
    //   TODO: your application logic goes here.
    //
    //   Some ideas to get you started:
    //
    //   1. Make BTN1 cycle the colour of RGB_LEDS[0]:
    //
    //       static uint8_t hue = 0;
    //       if (digitalRead(BTN1) == LOW) {
    //           hue += 32;
    //           RGB_LEDS[0] = CHSV(hue, 255, 255);
    //           FastLED.show();
    //           delay(BTN_DEBOUNCE_MS);
    //       }
    //
    //   2. Light an indicator LED while a magnet is held over the sensor:
    //
    //       CLED_State[4] = (digitalRead(HALL) == 0) ? 1 : 0;
    //
    //   3. Map the ambient-light reading onto LED brightness:
    //
    //       int v = analogRead(LUX);                       // 0..4095
    //       FastLED.setBrightness(map(v, 0, 4095, 5, 100));
    //       FastLED.show();
    //
    //   4. Print a heartbeat to the serial console once a second:
    //
    //       static unsigned long lastTick = 0;
    //       if (millis() - lastTick >= 1000) {
    //           lastTick = millis();
    //           ESP_LOGI(TAG, "tick (millis=%lu)", millis());
    //       }
    // ─────────────────────────────────────────────────────────────────────
}

// =============================================================================
// app_main — ESP-IDF entry point.  Calls Arduino setup() + loop() for you.
// =============================================================================
extern "C" void app_main() {
    initArduino();
    setup();
    for (;;) {
        loop();
    }
}
