// =============================================================================
//  main.cpp — BSides Vancouver Badge firmware
// =============================================================================
//
//  Entry point of the badge firmware. The badge is an ESP32-C3 with:
//
//   - ST25R3911B NFC reader (13.56 MHz, ISO-15693 / NFC-V) on SPI
//   - 5 × WS2812B-style addressable RGB LEDs (FastLED, RMT5 backend)
//   - 6 × monochrome LEDs driven through a 3-pin charlieplex matrix
//   - MGH201A1T3 bipolar-latch Hall-effect sensor
//   - ALS-PT19-315C ambient light sensor (analog)
//   - 2 × tactile pushbuttons (BTN1, BTN2)
//
//  At boot the firmware restores any persisted state from NVS, runs a
//  one-time component self-test (only on the very first boot), then enters
//  the main loop:
//
//   - Polls the Hall and lux sensors.
//   - Refreshes the LED display (RGB + charlieplex).
//   - Polls BTN1 (NFC tag scan trigger) and BTN2 (activity / long-press /
//     wake from light sleep).
//   - Services the RFAL NFC stack.
//   - Drops into light sleep after a configurable inactivity period, or
//     when the lux sensor reports dark for several seconds.
//
//  NFC tag command set — first character of the NDEF text payload selects
//  the action:
//
//     R / G / B / Y / P / O   set one of the preset team colours
//     C                       set a custom RGB team colour (Crrrgggbbb)
//     T                       record presentation attendance
//     V                       Malware Village achievement LED
//     L                       set global LED brightness (Lddd)
//     X                       wipe all saved badge state
//
//  Long-press BTN2 (≥ 2 s) toggles the optional WiFi access point + HTTP
//  control panel + captive-portal mini CTF (see wifi_portal.cpp).
//
//  Companion source files:
//
//     project_pins.h        — hardware pin map and tunable constants
//     badge_state.h/.cpp    — NVS-backed persistent state
//     wifi_portal.h/.cpp    — optional WiFi AP + web UI + CTF
//
// =============================================================================

#include "Arduino.h"
#include "FastLED.h"                          // RGB LED control
#include "nfc_utils.h"                        // NFC
#include "rfal_nfc.h"                         // NFC
#include "rfal_rfst25r3911.h"                 // NFC
#include "rfal_rfst25r3911_analogConfig.h"    // analog tuning for NFC
#include "ndef_class.h"                       // NFC
#include "esp_sleep.h"                        // light sleep + GPIO wakeup
#include "st25r3911_com.h"                    // direct ST25R3911B register writes
#include "driver/gpio.h"
#include "esp_log.h"

#include "project_pins.h"                     // pins, LED counts, thresholds
#include "badge_state.h"                      // NVS-backed persistent state
#include "wifi_portal.h"                      // optional WiFi AP + web UI + CTF


// -----------------------------------------------------------------------------
// LED state abstraction (used in many places below)
// -----------------------------------------------------------------------------
#define ON                 1
#define OFF                0


// -----------------------------------------------------------------------------
// Charlieplexed LED slot aliases (indexes into CLED_State[])
// -----------------------------------------------------------------------------
#define Presentation_1     0    // D3 — presentation-attendance counter bit 1
#define Presentation_3     1    // D4 — presentation-attendance counter bit 3
#define Malware_Village    2    // D5 — Malware Village attendance
#define Presentation_2     3    // D7 — presentation-attendance counter bit 2
#define HALL_LED           4    // D8 — hall-sensor puzzle solved indicator
#define LUX_LED            5    // D9 — lux-sensor puzzle solved indicator

uint8_t CLED_State[NUM_CLEDS];
uint8_t CLED_Save_State[NUM_CLEDS];


// -----------------------------------------------------------------------------
// RGB LED slot aliases (indexes into RGB_LEDS[])
// -----------------------------------------------------------------------------
//   Slot 0 — HALL_RGB        (status, sensor-driven)
//   Slot 1 — Team_Colour1    (user-selectable team colour)
//   Slot 2 — LUX_RGB         (status, sensor-driven)
//   Slot 3 — reserved        (intentionally unassigned)
//   Slot 4 — Team_Colour2    (user-selectable team colour; NUM_LEDS == 5 only)
//
#define HALL_RGB           0
#define Team_Colour1       1
#define LUX_RGB            2
#define Team_Colour2       4

CRGB RGB_LEDS[NUM_LEDS];
CRGB RGB_LEDS_Save_State[NUM_LEDS];


// -----------------------------------------------------------------------------
// Logging tag
// -----------------------------------------------------------------------------
static const char* TAG = "Badge";


// -----------------------------------------------------------------------------
// Sensor state machines
// -----------------------------------------------------------------------------
// Both sensors share the same shape: wait until the sensor is engaged
// continuously for HALL_HOLD_DURATION_MS / LUX_HOLD_DURATION_MS, then fire.
enum HallState { WAIT_HALL_ON1, TRIGGER_HALL };
enum LuxState  { WAIT_LUX_ON1,  TRIGGER_LUX  };

static HallState     hallState   = WAIT_HALL_ON1;
static unsigned long hallTsStart = 0;

static LuxState      luxState    = WAIT_LUX_ON1;
static unsigned long luxTsStart  = 0;


// -----------------------------------------------------------------------------
// NFC state machine (drives the switch() inside loop())
// -----------------------------------------------------------------------------
enum BadgeFsm {
    BADGE_NOTINIT          = 0,   // initial state at boot before RFAL is set up
    BADGE_START_DISCOVERY  = 1,   // BTN1 was pressed — kick off an NFC scan
    BADGE_DISCOVERY        = 2,   // NFC scan is running, waiting for a tag
    BADGE_WAIT_FOR_BUTTON  = 3    // idle, polling sensors and buttons
};

// NFC scan timing
#define DEMO_RAW_MESSAGE_BUF_LEN      8192
#define NDEF_READ_TIMEOUT             10000U   // ms — give up if no tag found


// -----------------------------------------------------------------------------
// Sleep management
// -----------------------------------------------------------------------------
static int64_t lastActivityTime = 0;          // microseconds since boot
static bool    isSleeping       = false;      // re-entry guard


// -----------------------------------------------------------------------------
// Live NFC state
// -----------------------------------------------------------------------------
static rfalNfcDiscoverParam discParam;
static uint8_t              state = BADGE_NOTINIT;
static uint8_t              rawMessageBuf[DEMO_RAW_MESSAGE_BUF_LEN];
uint8_t                     presentationCounter = 0;          // referenced by badge_state.cpp
uint8_t                     presentationTracker[8] = {0};     // referenced by badge_state.cpp

static uint32_t             timer;            // NFC read-mode timeout


// -----------------------------------------------------------------------------
// SPI bus + NFC driver chain
// -----------------------------------------------------------------------------
static const int spiClk = 1000000;            // 1 MHz
SPIClass dev_spi(0);
RfalRfST25R3911BClass rfst25r3911b(&dev_spi, CS_PIN, IRQ_PIN);
RfalNfcClass          rfal_nfc(&rfst25r3911b);
NdefClass             ndef(&rfal_nfc);


// -----------------------------------------------------------------------------
// Static tables (replace several large switch statements from v1)
// -----------------------------------------------------------------------------

// Team-colour command codes. The leading byte of the NDEF payload selects one.
static const struct {
    char       cmd;
    CRGB       colour;
    const char *label;
} TEAM_COLOURS[] = {
    {'R', CRGB::Red,    "Red"},
    {'G', CRGB::Green,  "Green"},
    {'B', CRGB::Blue,   "Blue"},
    {'Y', CRGB::Yellow, "Yellow"},
    {'P', CRGB::Purple, "Purple"},
    {'O', CRGB::Orange, "Orange"},
};
static constexpr size_t NUM_TEAM_COLOURS = sizeof(TEAM_COLOURS) / sizeof(TEAM_COLOURS[0]);

// Presentation codes. Each unique 9-byte string represents one presentation;
// reading it marks the corresponding tracker bit.
static const char* PRESENTATION_CODES[8] = {
    "aG7kL9vP2",  // presentation 1
    "Xd4Rj6HtQ",  // presentation 2
    "b3ZsT8nYw",  // presentation 3
    "M5yNq1cF8",  // presentation 4
    "pL2xQ7vD3",  // presentation 5
    "Rj9Wm4aS6",  // presentation 6
    "tU8kB1pF5",  // presentation 7
    "Qq3Zr7Yh2",  // presentation 8
};

// Other single-string command codes (fixed-length match).
static const char MALWARE_CODE[]    = "H2pG9wX3s";
static const char BADGE_WIPE_CODE[] = "BADGEWIPE";

// Charlieplex drive matrix.
//
// For each of the 6 LEDs, three values describe how X1, X2, X3 must be
// configured. The original v1 had 6 nearly-identical hand-written functions
// (LED1..LED6); this table replaces them.
enum PinDrive { TRI = 0, HIGH_OUT = 1, LOW_OUT = 2 };

static const PinDrive CLED_TABLE[NUM_CLEDS][3] = {
    /* LED1 / D3 */ {HIGH_OUT, LOW_OUT,  TRI     },
    /* LED2 / D4 */ {TRI,      HIGH_OUT, LOW_OUT },
    /* LED3 / D5 */ {HIGH_OUT, TRI,      LOW_OUT },
    /* LED4 / D7 */ {LOW_OUT,  HIGH_OUT, TRI     },
    /* LED5 / D8 */ {TRI,      LOW_OUT,  HIGH_OUT},
    /* LED6 / D9 */ {LOW_OUT,  TRI,      HIGH_OUT},
};

// Presentation-counter to LED-pattern lookup: how many presentations attended
// → which of Presentation_1/_2/_3 should light up. v1 had a 24-line switch
// statement; this is the same data in table form.
struct PresPattern { uint8_t l1, l2, l3; };
static const PresPattern PRES_PATTERNS[8] = {
    /* 0 attended */ {OFF, OFF, OFF},
    /* 1 attended */ {ON,  OFF, OFF},
    /* 2 attended */ {OFF, ON,  OFF},
    /* 3 attended */ {ON,  ON,  OFF},
    /* 4 attended */ {OFF, OFF, ON },
    /* 5 attended */ {ON,  OFF, ON },
    /* 6 attended */ {OFF, ON,  ON },
    /* 7 attended */ {ON,  ON,  ON },
};


// -----------------------------------------------------------------------------
// NDEF dispatch table (kept identical to v1 — RTD_TEXT is the only handler)
// -----------------------------------------------------------------------------
typedef struct {
    ndefTypeId typeId;
    ReturnCode(*dump)(const ndefType *type);
} ndefTypeDumpTable;


// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void enter_and_handle_light_sleep();

static void demoNdef(rfalNfcDevice *nfcDevice);

static void componentCheck(void);
static void CLEDDisplay(void);
static void lightCharlieplexLed(uint8_t idx);
static void saveLEDState(void);
static void restoreLEDState(void);
static void updatePresentationCounter(void);
static void startupLights(void);

static ReturnCode ndefRecordDump(const ndefRecord *record);
static ReturnCode ndefMessageDump(const ndefMessage *message);
static ReturnCode ndefRtdTextDump(const ndefType *text);
static ReturnCode ndefRecordDumpType(const ndefRecord *record);
static ReturnCode ndefReadTag(const ndefConstBuffer *bufPayload);

static void checkButtons(void);
static void checkHallEffectSensor(void);
static void checkLuxSensor(void);
static void checkAndApplyMasterUnlock(void);

// -----------------------------------------------------------------------------
// Game flags
// -----------------------------------------------------------------------------
// BSides conference orange — used as the master-unlock indicator on RGB[3]
// once the attendee has collected every achievement.
static const CRGB  MASTER_UNLOCK_COLOUR = CRGB(238, 106, 37);

static const ndefTypeDumpTable typeDumpTable[] = {
    { NDEF_TYPE_ID_RTD_TEXT, ndefRtdTextDump }
};


// =============================================================================
// Sleep management
// =============================================================================
static void enter_and_handle_light_sleep() {
    esp_err_t err;

    if (isSleeping) return;     // re-entry guard

    ESP_LOGI(TAG, "Preparing for light sleep. Waking on BTN2 (GPIO %d) LOW.", BTN2);
    isSleeping = true;

    // Visual cue: red on slot 0 while preparing to sleep.
    RGB_LEDS[0] = CRGB::Red;
    FastLED.show();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN2),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,    // rely on external pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE       // level-triggered, not edge
    };
    gpio_config(&io_conf);

    // Wake sources: BTN2 (RTC-capable GPIO 2) AND the lux sensor on GPIO 1
    // (also RTC-capable). The lux wake-on-bright closes the pocket-sleep
    // loop: dark for LUX_POCKET_SLEEP_MS triggers sleep, returning light
    // wakes it back up.
    if (BTN2 == GPIO_NUM_2) {
        err = gpio_wakeup_enable(BTN2, GPIO_INTR_LOW_LEVEL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable BTN2 wakeup: %s", esp_err_to_name(err));
            isSleeping = false;
            RGB_LEDS[0] = CRGB::Green;
            FastLED.show();
            return;
        }

        err = gpio_wakeup_enable((gpio_num_t)LUX, GPIO_INTR_HIGH_LEVEL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable LUX wakeup: %s", esp_err_to_name(err));
            // Non-fatal — BTN2 wake still works.
        }

        err = esp_sleep_enable_gpio_wakeup();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable general GPIO wakeup source: %s", esp_err_to_name(err));
            gpio_wakeup_disable(BTN2);
            gpio_wakeup_disable((gpio_num_t)LUX);
            isSleeping = false;
            RGB_LEDS[0] = CRGB::Green;
            FastLED.show();
            return;
        }
    }

    ESP_LOGI(TAG, "Entering light sleep (wakes on BTN2 press or returning light)...");
    err = esp_light_sleep_start();

    // --- Execution resumes here after wakeup ---
    gpio_wakeup_disable(BTN2);
    gpio_wakeup_disable((gpio_num_t)LUX);

    isSleeping = false;
    RGB_LEDS[0] = CRGB::Green;      // back to awake colour
    FastLED.show();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error during light sleep / wakeup: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Woke up from light sleep!");
    }

    lastActivityTime = esp_timer_get_time();

    // Try to identify the wake source by sampling the relevant pins now.
    // The user may have already released BTN2 / re-covered the lux sensor
    // before we read, so this is best-effort.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        if (digitalRead(BTN2) == LOW) {
            ESP_LOGI(TAG, "Wakeup caused by BTN2.");
        } else if (digitalRead(LUX) == HIGH) {
            ESP_LOGI(TAG, "Wakeup caused by light returning (out of pocket).");
        } else {
            ESP_LOGI(TAG, "Wakeup caused by GPIO (source already changed).");
        }
    } else {
        ESP_LOGI(TAG, "Wakeup cause: %d", cause);
    }

    FastLED.show();

#if !BADGE_LOW_POWER
    // Re-initialize the NFC reader after wake. During light sleep the
    // ESP32-C3 stops servicing SPI and the ST25R3911B's protocol state
    // machine drifts out of sync with what RFAL thinks is going on; without
    // this re-init, NFC tag scans silently fail after the first sleep.
    // Cost is ~tens of ms; users won't notice.
    ESP_LOGI(TAG, "Re-initializing NFC reader after wake...");
    ReturnCode reErr = rfal_nfc.rfalNfcInitialize();
    if (reErr == ERR_NONE) {
        rfst25r3911b.st25r3911WriteRegister(0x2A, 0xF8);   // restore max TX power
        state = BADGE_WAIT_FOR_BUTTON;
        ESP_LOGI(TAG, "NFC re-init OK");
    } else {
        ESP_LOGW(TAG, "NFC re-init ERROR: %i — tag scans may fail until next reboot", reErr);
    }
#endif
}


// =============================================================================
// Entry points: app_main / setup / loop
// =============================================================================
extern "C" void app_main()
{
    initArduino();
    setup();
    for (;;) {
        loop();
    }
}

void setup()
{
    // In low-power mode, drop the CPU clock immediately. Default is 160 MHz
    // (~22 mA core); 80 MHz cuts that roughly in half (~14 mA) with no impact
    // on FastLED, the charlieplex refresh, or button polling. We don't initialise
    // NFC in low-power mode anyway, so the SPI/NFC timing dependency goes away.
#if BADGE_LOW_POWER
    setCpuFrequencyMhz(80);
#endif

    ESP_LOGW(TAG, "CPU frequency: %li MHz", getCpuFrequencyMhz());

    // Log the reason for the most recent reset. Useful when running on battery:
    // a "BROWNOUT" reason here proves the chip is resetting because the supply
    // voltage dipped below the brownout threshold (~2.51 V on ESP32-C3) — i.e.
    // the coin cells can't keep up with a current spike (NFC TX, FastLED, etc.).
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* name;
        switch (rr) {
            case ESP_RST_POWERON:    name = "POWERON";    break;
            case ESP_RST_EXT:        name = "EXT (reset pin)"; break;
            case ESP_RST_SW:         name = "SW (esp_restart)"; break;
            case ESP_RST_PANIC:      name = "PANIC";      break;
            case ESP_RST_INT_WDT:    name = "INT_WDT";    break;
            case ESP_RST_TASK_WDT:   name = "TASK_WDT";   break;
            case ESP_RST_WDT:        name = "WDT (other)";break;
            case ESP_RST_DEEPSLEEP:  name = "DEEPSLEEP wake"; break;
            case ESP_RST_BROWNOUT:   name = "BROWNOUT";   break;
            case ESP_RST_SDIO:       name = "SDIO";       break;
            default:                 name = "UNKNOWN";    break;
        }
        ESP_LOGW(TAG, "Reset reason: %d (%s)", (int)rr, name);
    }

    // Silence ESP-IDF's gpio component INFO logs. Every pinMode() call below
    // (and inside CLEDDisplay() at runtime) otherwise produces a verbose
    // "GPIO[NN]| InputEn: ..." line that drowns out our own Badge: logs.
    esp_log_level_set("gpio", ESP_LOG_WARN);

    // -- Hard-reset shortcut --------------------------------------------------
    // BTN2 (GPIO 2) held down at boot wipes all NVS state. Matches production
    // firmware's behaviour and gives a recovery path for a badge in a stuck
    // state. Must run BEFORE BadgeState::restoreAll() so the wiped state is
    // what gets loaded.
    pinMode(RGB_LED_PIN, OUTPUT);
    pinMode(BTN2, INPUT);
    FastLED.addLeds<NEOPIXEL, RGB_LED_PIN>(RGB_LEDS, NUM_LEDS);
    // Keep initial brightness very low — at 30/255 the five WS2812B LEDs can
    // pull ~300 mA briefly on the BTN2-hard-reset flash, which is more than
    // two CR2032 cells in parallel can supply (→ brownout reset loop).
    FastLED.setBrightness(BADGE_LOW_POWER ? BADGE_BRIGHTNESS_BOOT_LP : BADGE_BRIGHTNESS_BOOT);
    if (digitalRead(BTN2) == LOW) {
        ESP_LOGW(TAG, "BTN2 pressed at startup: Performing HARD RESET. "
                      "Please release the button.");
        BadgeState::wipeAll();
        for (int i = 0; i < NUM_LEDS; i++) RGB_LEDS[i] = CRGB::Red;
        FastLED.show();
        while (digitalRead(BTN2) == LOW) delay(BTN_DEBOUNCE_MS);
        for (int i = 0; i < NUM_LEDS; i++) RGB_LEDS[i] = CRGB::Black;
        FastLED.show();
        ESP_LOGW(TAG, "HARD RESET complete. All persisted state has been erased.");
    }
    FastLED.setBrightness(BADGE_BRIGHTNESS_DEFAULT);

    ReturnCode err;

    dev_spi.begin();
    ESP_LOGW(TAG, "SPI started");

    // GPIO directions for the remaining pins (RGB_LED_PIN and BTN2 already
    // configured above for the hard-reset shortcut).
    pinMode(X1, OUTPUT);
    pinMode(X2, OUTPUT);
    pinMode(X3, OUTPUT);
    pinMode(BTN1, INPUT);
    // INPUT_PULLUP on HALL: the MGH201A1T3 has an open-drain output that pulls
    // the line LOW when a magnet is detected. Without an internal pull-up the
    // idle state can float and be misread as ACTIVE. The production schematic
    // has an external pull-up, but enabling the internal one too is harmless
    // and makes development boards without that resistor behave correctly.
    pinMode(HALL, INPUT_PULLUP);
    pinMode(LUX, INPUT);
    FastLED.show();

    // Startup eye-candy.
    startupLights();

    // Restore everything that was saved before the last power loss.
    BadgeState::restoreAll();

    // If all attendance challenges are already done, ensure the master
    // unlock LED is showing on boot too.
    checkAndApplyMasterUnlock();

    if (!isSleeping) {
        RGB_LEDS[0] = CRGB::Green;    // awake indicator
    }
    FastLED.show();

    // One-time self-test (only runs on the very first boot ever — gated by NVS).
    componentCheck();

    lastActivityTime = esp_timer_get_time();

    ESP_LOGW(TAG, "Welcome to the BSides 2025 Badge!!!");

#if BADGE_LOW_POWER
    // -- NFC initialization SKIPPED in low-power mode -------------------------
    // The ST25R3911B's oscillator + analog front-end inrush is the single
    // biggest current spike in the boot sequence and reliably crashes a pair
    // of CR2032 cells in parallel. In low-power / battery-test mode we leave
    // NFC offline; BTN1 becomes a no-op, but the rest of the badge (sensors,
    // LEDs, sleep) works normally. Flip BADGE_LOW_POWER to 0 to restore NFC.
    ESP_LOGW(TAG, "BADGE_LOW_POWER=1 — NFC initialization SKIPPED (battery mode)");
    state = BADGE_WAIT_FOR_BUTTON;
    (void)err;
#else
    // Workaround: explicitly set the NDEF back-pointer. On some build
    // configurations the constructor's write to `ctx.ndef_class_instance`
    // is elided or overwritten before the first NFC scan, leading to a
    // Guru Meditation fault inside ndefT5TPollerAccessMode at line 149.
    ndef.initBackPointer();

    err = rfal_nfc.rfalNfcInitialize();
    rfst25r3911b.st25r3911WriteRegister(0x2A, 0xF8);  // max NFC TX amp power

    if (err == ERR_NONE) {
        ESP_LOGW(TAG, "Initialize successful");
        discParam.compMode            = RFAL_COMPLIANCE_MODE_NFC;
        discParam.devLimit            = 1U;
        discParam.nfcfBR              = RFAL_BR_212;
        discParam.ap2pBR              = RFAL_BR_424;
        discParam.notifyCb            = NULL;
        discParam.totalDuration       = 1000U;
        discParam.wakeupEnabled       = true;
        discParam.wakeupConfigDefault = true;
        discParam.techs2Find          = RFAL_NFC_POLL_TECH_V;

        state = BADGE_WAIT_FOR_BUTTON;
    } else {
        ESP_LOGW(TAG, "Initialize ERROR: %i", err);
    }
#endif
}

void loop()
{
    static rfalNfcDevice *nfcDevice;
    rfalNfcvInventoryRes  invRes;
    uint16_t              rcvdLen;

    // Refresh awake-indicator LED if we're not currently sleeping.
    // Green = awake & idle; Blue = awake & WiFi portal active.
    if (BTN2 == GPIO_NUM_2 && !isSleeping) {
        CRGB target = wifi_portal_is_active() ? CRGB(CRGB::Blue) : CRGB(CRGB::Green);
        if (RGB_LEDS[0] != target) {
            RGB_LEDS[0] = target;
            FastLED.show();
        }
    }

    // CTF flag captured? Light up the LEDs as a celebration.
    // The httpd handler sets the flag from its own task; we drain it from the
    // foreground so FastLED.show() isn't called from two tasks at once.
    if (wifi_portal_consume_flag()) {
        ESP_LOGW(TAG, "CTF flag captured — celebrating!");
        CRGB saved[NUM_LEDS];
        for (int i = 0; i < NUM_LEDS; i++) saved[i] = RGB_LEDS[i];
        for (int pulse = 0; pulse < CTF_PULSE_COUNT; pulse++) {
            CRGB c = (pulse & 1) ? CRGB::Green : CRGB::Black;
            for (int i = 0; i < NUM_LEDS; i++) RGB_LEDS[i] = c;
            FastLED.show();
            delay(CTF_PULSE_MS);
        }
        for (int i = 0; i < NUM_LEDS; i++) RGB_LEDS[i] = saved[i];
        FastLED.show();
    }

    checkHallEffectSensor();
    checkLuxSensor();
    CLEDDisplay();
    checkButtons();

    // Sleep triggers — only relevant on the production board (BTN2 on
    // RTC-capable pin can wake the chip).
    //
    // BADGE_DEBUG_NO_SLEEP gates the whole block off — see project_pins.h for
    // rationale. Tied to a #if (not an `if`) so the body is gone at compile
    // time and there's no chance of a stray sleep entry while debugging.
#if BADGE_DEBUG_NO_SLEEP
    {
        static bool warned = false;
        if (!warned) {
            ESP_LOGW(TAG, "BADGE_DEBUG_NO_SLEEP=1 — light sleep DISABLED for USB monitoring");
            warned = true;
        }
    }
#else
    // Suppress all sleep while the WiFi portal is active — light sleep would
    // tear down the AP and disconnect every client. The portal is only active
    // when USB power is plugged in anyway (battery can't sustain WiFi).
    if (BTN2 == GPIO_NUM_2 && !isSleeping && !wifi_portal_is_active()) {

        // (1) "In-pocket" sleep: lux sensor dark continuously for
        // LUX_POCKET_SLEEP_MS suggests the badge is in a pocket / dark bag.
        // Sleep immediately; returning light or BTN2 press will wake it.
        //
        // We use analogRead against LUX_DARK_THRESHOLD (not digitalRead) so
        // that ordinary indoor ambient light doesn't trigger sleep. The
        // digital threshold on GPIO 1 is ~1.5 V, which most room lighting
        // sits below — only an actually-covered sensor reads as dark.
        static unsigned long luxDarkStart = 0;
        bool luxDark = (analogRead(LUX) < LUX_DARK_THRESHOLD);
        if (luxDark) {
            if (luxDarkStart == 0) luxDarkStart = millis();
            if (millis() - luxDarkStart >= LUX_POCKET_SLEEP_MS) {
                ESP_LOGW(TAG, "Lux sensor dark for %d ms — entering pocket sleep.",
                         LUX_POCKET_SLEEP_MS);
                if (state != BADGE_WAIT_FOR_BUTTON) {
                    rfal_nfc.rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
                    state = BADGE_WAIT_FOR_BUTTON;
                    restoreLEDState();
                }
                enter_and_handle_light_sleep();
                luxDarkStart = 0;          // recompute after wake
            }
        } else {
            luxDarkStart = 0;
        }

        // (2) Inactivity-timeout sleep: no activity for INACTIVITY_TIMEOUT_S.
        // "Activity" = button press or sensor puzzle trigger. With lux-pocket
        // sleep doing most of the work, this is a fallback for the badge
        // sitting face-down (lux not dark, but no real interaction either).
        const int64_t inactivity_us = (int64_t)INACTIVITY_TIMEOUT_S * 1000000LL;
        if (esp_timer_get_time() - lastActivityTime > inactivity_us) {
            ESP_LOGI(TAG, "Inactivity detected. Preparing for light sleep.");
            if (state != BADGE_WAIT_FOR_BUTTON) {
                ESP_LOGI(TAG, "NFC discovery active, deactivating before sleep.");
                rfal_nfc.rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
                state = BADGE_WAIT_FOR_BUTTON;
                restoreLEDState();
            }
            enter_and_handle_light_sleep();
        }
    }
#endif  // BADGE_DEBUG_NO_SLEEP

#if !BADGE_LOW_POWER
    // NFC scan timed out without a tag — return to idle.
    if ((state != BADGE_WAIT_FOR_BUTTON) && (rfst25r3911b.timerIsExpired(timer))) {
        restoreLEDState();
        ESP_LOGW(TAG, "Timer Expired. Back to normal operations");
        state = BADGE_WAIT_FOR_BUTTON;
    }

    rfal_nfc.rfalNfcWorker();   // RFAL background tasks
#endif

    switch (state) {
        case BADGE_WAIT_FOR_BUTTON:
            break;

        case BADGE_START_DISCOVERY:
            rfal_nfc.rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
            rfal_nfc.rfalNfcDiscover(&discParam);
            state = BADGE_DISCOVERY;
            break;

        case BADGE_DISCOVERY:
            if (rfalNfcIsDevActivated(rfal_nfc.rfalNfcGetState())) {
                rfal_nfc.rfalNfcGetActiveDevice(&nfcDevice);
                delay(BTN_DEBOUNCE_MS);     // let the RFAL state settle

                ESP_LOGW(TAG, "ISO15693/NFC-V card found.");
                demoNdef(nfcDevice);

                // Block until the user removes the tag from the field.
                ESP_LOGW(TAG, "Operation completed - Tag can be removed from the field");
                rfal_nfc.rfalNfcvPollerInitialize();
                while (rfal_nfc.rfalNfcvPollerInventory(
                            RFAL_NFCV_NUM_SLOTS_1, RFAL_NFCV_UID_LEN * 8U,
                            nfcDevice->dev.nfcv.InvRes.UID, &invRes, &rcvdLen) == ERR_NONE) {
                    delay(NFC_TAG_REMOVE_POLL_MS);
                }

                rfal_nfc.rfalNfcDeactivate(RFAL_NFC_DEACTIVATE_IDLE);
                delay(NFC_POST_DEACTIVATE_MS);
                state = BADGE_WAIT_FOR_BUTTON;
                restoreLEDState();
            }
            break;

        case BADGE_NOTINIT:
        default:
            break;
    }
}


// =============================================================================
// NDEF read pipeline (top half — context init, detect, decode)
// =============================================================================
static void demoNdef(rfalNfcDevice *pNfcDevice)
{
    ReturnCode      err;
    ndefMessage     message;
    uint32_t        rawMessageLen;
    ndefInfo        info;
    ndefConstBuffer bufConstRawMessage;

    err = ndef.ndefPollerContextInitializationWrapper(pNfcDevice);
    if (err != ERR_NONE) {
        ESP_LOGW(TAG, "NDEF NOT DETECTED (ndefPollerContextInitializationWrapper returns %i)", err);
        return;
    }

    err = ndef.ndefPollerNdefDetectWrapper(&info);
    if (err != ERR_NONE) {
        ESP_LOGW(TAG, "NDEF NOT DETECTED (ndefPollerNdefDetectWrapper returns %i)", err);
    } else {
        ESP_LOGW(TAG, "NDEF detected.");
    }

    if (info.state == NDEF_STATE_INITIALIZED) {
        // The tag is initialized but blank.
        return;
    }

    err = ndef.ndefPollerReadRawMessageWrapper(rawMessageBuf, sizeof(rawMessageBuf),
                                               &rawMessageLen, true);
    if (err != ERR_NONE) {
        ESP_LOGW(TAG, "NDEF message cannot be read (returns %i)", err);
        return;
    }

    bufConstRawMessage.buffer = rawMessageBuf;
    bufConstRawMessage.length = rawMessageLen;

    err = ndefMessageDecode(&bufConstRawMessage, &message);
    if (err != ERR_NONE) {
        ESP_LOGW(TAG, "NDEF message cannot be decoded (returns %i)", err);
        return;
    }

    err = ndefMessageDump(&message);
    if (err != ERR_NONE) {
        ESP_LOGW(TAG, "NDEF message cannot be displayed (returns %i)", err);
        return;
    }
}


// =============================================================================
// First-boot component self-test
// =============================================================================
static void componentCheck(void)
{
    if (BadgeState::firstBootCheckComplete()) {
        return;
    }

    int64_t initTimer = esp_timer_get_time();

    ESP_LOGW(TAG, "Turn on lights for 5 seconds");
    for (int i = 0; i < NUM_CLEDS; i++)  CLED_State[i] = ON;
    for (int i = 0; i < NUM_LEDS;  i++)  RGB_LEDS[i]   = CRGB::Red;
    FastLED.show();

    // Hold for 5 s while servicing the charlieplex refresh.
    while (esp_timer_get_time() - initTimer < 5LL * 1000000LL) {
        CLEDDisplay();
        delay(1);
    }

    // Turn everything off.
    for (int i = 0; i < NUM_CLEDS; i++)  CLED_State[i] = OFF;
    for (int i = 0; i < NUM_LEDS;  i++)  RGB_LEDS[i]   = CRGB::Black;
    FastLED.show();

    // Wait for each input in turn.
    ESP_LOGW(TAG, "Check Hall Effect Sensor — present a magnet (HALL pin should read 0)");
    {
        unsigned long lastLog = 0;
        while (digitalRead(HALL) != 0) {
            unsigned long t = millis();
            if (t - lastLog >= 500) {
                lastLog = t;
                ESP_LOGI(TAG, "  HALL pin=%d (waiting for 0)", digitalRead(HALL));
            }
            delay(1);
        }
        ESP_LOGW(TAG, "Hall sensor OK — pin went LOW");
    }

    ESP_LOGW(TAG, "Check Lux Sensor — cover the sensor (analog should drop below 100)");
    {
        unsigned long lastLog = 0;
        int v;
        while ((v = analogRead(LUX)) > 100) {
            unsigned long t = millis();
            if (t - lastLog >= 500) {
                lastLog = t;
                ESP_LOGI(TAG, "  LUX analog=%d (waiting for <100)", v);
            }
            delay(1);
        }
        ESP_LOGW(TAG, "Lux sensor OK — analog=%d", v);
    }

    ESP_LOGW(TAG, "Check Button 1");
    while (digitalRead(BTN1) == HIGH) {
        while (digitalRead(BTN1) == LOW);
        delay(BTN_DEBOUNCE_MS);
    }

    if (BTN2 == GPIO_NUM_2) {
        ESP_LOGW(TAG, "Check Button 2");
        while (digitalRead(BTN2) == HIGH) {
            delay(100);
            while (digitalRead(BTN2) == LOW);
            delay(100);
        }
    }

    BadgeState::markFirstBootCheckComplete();
}


// =============================================================================
// LED helpers
// =============================================================================
// Drive a single charlieplexed LED according to CLED_TABLE.
// Each refresh cycle we sequentially light one LED, then move to the next.
static void lightCharlieplexLed(uint8_t idx)
{
    static const int pins[3] = {X1, X2, X3};

    for (int i = 0; i < 3; i++) {
        switch (CLED_TABLE[idx][i]) {
            case TRI:
                pinMode(pins[i], INPUT);
                break;
            case HIGH_OUT:
                pinMode(pins[i], OUTPUT);
                digitalWrite(pins[i], HIGH);
                break;
            case LOW_OUT:
                pinMode(pins[i], OUTPUT);
                digitalWrite(pins[i], LOW);
                break;
        }
    }
}

// One refresh pass: walk through all 6 charlieplexed LEDs, lighting each whose
// CLED_State[idx] == ON. Then return all three drive pins to high-impedance.
static void CLEDDisplay(void)
{
    for (int i = 0; i < NUM_CLEDS; i++) {
        if (CLED_State[i] == ON) {
            lightCharlieplexLed(i);
        }
        delay(CLED_Delay);
    }

    // Park all three pins as inputs to extinguish the matrix until next refresh.
    pinMode(X1, INPUT);
    pinMode(X2, INPUT);
    pinMode(X3, INPUT);
}

// Snapshot the current visible LED state into the *_Save_State buffers, so we
// can blank or repurpose the LEDs during an NFC scan and restore on exit.
static void saveLEDState(void)
{
    for (int i = 0; i < NUM_LEDS;  i++) RGB_LEDS_Save_State[i] = RGB_LEDS[i];
    for (int i = 0; i < NUM_CLEDS; i++) CLED_Save_State[i]     = CLED_State[i];
}

static void restoreLEDState(void)
{
    for (int i = 0; i < NUM_LEDS;  i++) RGB_LEDS[i]   = RGB_LEDS_Save_State[i];
    FastLED.show();
    for (int i = 0; i < NUM_CLEDS; i++) CLED_State[i] = CLED_Save_State[i];
}


// =============================================================================
// Presentation-attendance counter
// =============================================================================
// Increments the counter and updates the three indicator LEDs according to a
// lookup table (PRES_PATTERNS) — replaces a 24-line switch in v1.
static void updatePresentationCounter(void)
{
    presentationCounter++;
    ESP_LOGW(TAG, "Presentation counter is: %i", presentationCounter);

    if (presentationCounter < (sizeof(PRES_PATTERNS) / sizeof(PRES_PATTERNS[0]))) {
        const PresPattern& p = PRES_PATTERNS[presentationCounter];
        CLED_Save_State[Presentation_1] = p.l1;
        CLED_Save_State[Presentation_2] = p.l2;
        CLED_Save_State[Presentation_3] = p.l3;
    }
}


// =============================================================================
// Startup animation
// =============================================================================
static void startupLights(void)
{
    static uint8_t hue = 0;
    FastLED.setBrightness(BADGE_BRIGHTNESS_DEFAULT);

    // Roll a hue around the RGB chain, one LED at a time.
    for (int i = 0; i < NUM_LEDS; i++) {
        RGB_LEDS[i] = CHSV(hue++, 255, 255);
        FastLED.show();
        delay(1000);
    }

    // Then sweep each charlieplexed LED, 100 refresh cycles each.
    // (v1 had a variable-shadowing bug here where the inner loop reused `i`.)
    for (int led_idx = 0; led_idx < NUM_CLEDS; led_idx++) {
        CLED_State[led_idx] = ON;
        for (int cycle = 0; cycle < 100; cycle++) {
            CLEDDisplay();
        }
    }
}


// =============================================================================
// NDEF parsing helpers
// =============================================================================
ReturnCode ndefRecordDump(const ndefRecord *record)
{
    static uint32_t index;

    if (record == NULL) {
        ESP_LOGW(TAG, "No record");
        return ERR_NONE;
    }

    if (ndefHeaderIsSetMB(record)) {
        index = 1U;
    } else {
        index++;
    }

    ndefRecordDumpType(record);
    return ERR_NONE;
}

ReturnCode ndefMessageDump(const ndefMessage *message)
{
    ReturnCode  err;
    ndefRecord *record;

    if (message == NULL) {
        ESP_LOGW(TAG, "Empty NDEF message");
        return ERR_NONE;
    }
    ESP_LOGW(TAG, "Decoding NDEF message");

    record = ndefMessageGetFirstRecord(message);
    while (record != NULL) {
        err = ndefRecordDump(record);
        if (err != ERR_NONE) {
            return err;
        }
        record = ndefMessageGetNextRecord(record);
    }
    return ERR_NONE;
}

ReturnCode ndefRtdTextDump(const ndefType *type)
{
    uint8_t          utfEncoding;
    ndefConstBuffer8 bufLanguageCode;
    ndefConstBuffer  bufSentence;
    ReturnCode err = ndefGetRtdText(type, &utfEncoding, &bufLanguageCode, &bufSentence);
    if (err != ERR_NONE) {
        return err;
    }
    ndefReadTag(&bufSentence);
    return ERR_NONE;
}

ReturnCode ndefRecordDumpType(const ndefRecord *record)
{
    ReturnCode err;
    ndefType   type;

    err = ndefRecordToType(record, &type);
    if (err != ERR_NONE) {
        return err;
    }

    for (size_t i = 0; i < SIZEOF_ARRAY(typeDumpTable); i++) {
        if (type.id == typeDumpTable[i].typeId && typeDumpTable[i].dump != NULL) {
            return typeDumpTable[i].dump(&type);
        }
    }
    return ERR_NOT_IMPLEMENTED;
}


// =============================================================================
// Command dispatch for an NDEF-encoded text payload
//
// The first byte of the payload selects a command; bytes 1..9 carry the
// 9-byte argument (presentation code, brightness digits, RGB digits, etc.).
// =============================================================================
ReturnCode ndefReadTag(const ndefConstBuffer *bufString)
{
    if (bufString == NULL || bufString->buffer == NULL) {
        ESP_LOGW(TAG, "Reading Tag: <no buffer>");
        return ERR_PARAM;
    }
    if (bufString->length < 1) {
        ESP_LOGW(TAG, "Reading Tag: empty payload");
        return ERR_NONE;
    }

    const char cmd = (char)bufString->buffer[0];
    ESP_LOGW(TAG, "Reading Tag: command='%c' length=%lu", cmd,
             (unsigned long)bufString->length);

    // ----- Team-colour preset commands ('R','G','B','Y','P','O') -----------
    // These need only the 1-byte command — no arguments.
    for (size_t i = 0; i < NUM_TEAM_COLOURS; i++) {
        if (cmd == TEAM_COLOURS[i].cmd) {
            const CRGB c = TEAM_COLOURS[i].colour;
            ESP_LOGW(TAG, "CTF Color: R=%d, G=%d, B=%d  (%s team)",
                     c.r, c.g, c.b, TEAM_COLOURS[i].label);
            RGB_LEDS_Save_State[Team_Colour1] = c;
            if (NUM_LEDS == 5) {
                RGB_LEDS_Save_State[Team_Colour2] = c;
            }
            BadgeState::saveTeamColours();
            return ERR_NONE;
        }
    }

    // All remaining commands require a 9-byte argument after the command
    // byte. Reject early if the payload is too short.
    if (bufString->length <= 9) {
        ESP_LOGW(TAG, "Tag command '%c' requires a 9-byte argument — got %lu bytes",
                 cmd, (unsigned long)bufString->length);
        return ERR_NONE;
    }

    uint8_t tagInfo[10];
    memcpy(tagInfo, bufString->buffer + 1, 9);

    switch (cmd) {

        // ----- Custom RGB colour: "Crrrgggbbb" (decimal digits) -------------
        case 'C': {
            auto digit  = [](uint8_t c) -> int { return c - '0'; };
            auto clamp8 = [](int v) { return (v < 0) ? 0 : (v > 255) ? 255 : v; };

            int r_val = clamp8(digit(bufString->buffer[1]) * 100 + digit(bufString->buffer[2]) * 10 + digit(bufString->buffer[3]));
            int g_val = clamp8(digit(bufString->buffer[4]) * 100 + digit(bufString->buffer[5]) * 10 + digit(bufString->buffer[6]));
            int b_val = clamp8(digit(bufString->buffer[7]) * 100 + digit(bufString->buffer[8]) * 10 + digit(bufString->buffer[9]));

            ESP_LOGW(TAG, "Custom Color: R=%d, G=%d, B=%d", r_val, g_val, b_val);

            RGB_LEDS_Save_State[Team_Colour1].setRGB(r_val, g_val, b_val);
            RGB_LEDS_Save_State[Team_Colour2].setRGB(r_val, g_val, b_val);
            BadgeState::saveTeamColours();
            break;
        }

        // ----- Presentation attendance --------------------------------------
        case 'T': {
            char code[10] = {0};
            memcpy(code, tagInfo, 9);
            ESP_LOGW(TAG, "Presentation code received: %s", code);
            for (int i = 0; i < 8; i++) {
                if (memcmp(PRESENTATION_CODES[i], tagInfo, 9) == 0) {
                    if (presentationTracker[i] == 0) {
                        ESP_LOGW(TAG, "Attended presentation %d!", i + 1);
                        presentationTracker[i] = 1;
                        updatePresentationCounter();
                        BadgeState::savePresentationProgress();
                        checkAndApplyMasterUnlock();
                    } else {
                        ESP_LOGW(TAG, "Presentation %d already recorded", i + 1);
                    }
                    return ERR_NONE;
                }
            }
            ESP_LOGW(TAG, "Unknown presentation code: %s", code);
            break;
        }

        // ----- Malware Village ----------------------------------------------
        case 'V': {
            char code[10] = {0};
            memcpy(code, tagInfo, 9);
            ESP_LOGW(TAG, "Malware Village code received: %s", code);
            if (memcmp(MALWARE_CODE, tagInfo, 9) == 0) {
                ESP_LOGW(TAG, "Attended Malware Village!");
                CLED_Save_State[Malware_Village] = ON;
                CLED_State[Malware_Village] = ON;
                BadgeState::saveMalwareFlag();
                checkAndApplyMasterUnlock();
            } else {
                ESP_LOGW(TAG, "Invalid Malware Village code");
            }
            break;
        }

        // ----- Badge wipe ---------------------------------------------------
        case 'X': {
            char code[10] = {0};
            memcpy(code, tagInfo, 9);
            ESP_LOGW(TAG, "Badge wipe command received: %s", code);
            if (memcmp(BADGE_WIPE_CODE, tagInfo, 9) == 0) {
                ESP_LOGW(TAG, "--- WIPING ALL BADGE DATA ---");
                // Zero every charlieplex LED — both live state and save
                // state. Previously only Malware_Village's live state was
                // cleared, so D7/D8/D9/D3/D4 could remain visibly lit
                // until the next restoreLEDState() pass.
                for (int i = 0; i < NUM_CLEDS; i++) {
                    CLED_State[i]      = OFF;
                    CLED_Save_State[i] = OFF;
                }
                presentationCounter = 0;
                for (int i = 0; i < 8; i++) presentationTracker[i] = 0;
                // Zero every RGB slot — both live state and save state.
                for (int i = 0; i < NUM_LEDS; i++) {
                    RGB_LEDS[i]            = CRGB::Black;
                    RGB_LEDS_Save_State[i] = CRGB::Black;
                }

                // RGB[3] master-unlock indicator was already zeroed by the
                // loop above. We just need to flush the saved state to NVS.
                FastLED.setBrightness(BADGE_BRIGHTNESS_DEFAULT);
                FastLED.show();
                BadgeState::saveMalwareFlag();
                BadgeState::saveHallFlag();
                BadgeState::saveLuxFlag();
                BadgeState::savePresentationProgress();
                BadgeState::saveTeamColours();
                BadgeState::saveRgb4();   // clears RGB#3 colour in NVS
                ESP_LOGW(TAG, "--- BADGE WIPE COMPLETE ---");
            } else {
                ESP_LOGW(TAG, "Invalid badge-wipe code");
            }
            break;
        }

        // ----- Set brightness: "Lddd" (decimal digits) ----------------------
        case 'L': {
            int b = (bufString->buffer[1] - '0') * 100
                  + (bufString->buffer[2] - '0') * 10
                  + (bufString->buffer[3] - '0');
            b = (b < 0) ? 0 : (b > 255) ? 255 : b;
            ESP_LOGW(TAG, "Setting brightness to %d", b);
            FastLED.setBrightness(b);
            BadgeState::saveBrightness();
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown tag command '%c' — ignored", cmd);
            break;
    }

    return ERR_NONE;
}


// =============================================================================
// Button polling
// =============================================================================
static void checkButtons(void)
{
    // BTN1 — kick off an NFC tag read.
    if (digitalRead(BTN1) == LOW) {
        delay(BTN_DEBOUNCE_MS);                     // debounce press
        while (digitalRead(BTN1) == LOW);           // wait for release
        delay(BTN_DEBOUNCE_MS);                     // debounce release
        lastActivityTime = esp_timer_get_time();
#if BADGE_LOW_POWER
        // NFC stack isn't initialised in low-power mode; calling
        // rfst25r3911b/rfal_nfc here would crash. Log and bail.
        ESP_LOGW(TAG, "BTN1 pressed — NFC disabled in BADGE_LOW_POWER mode.");
#else
        timer = rfst25r3911b.timerCalculateTimer(NDEF_READ_TIMEOUT);
        saveLEDState();
        state = BADGE_START_DISCOVERY;
#endif
    }

    // BTN2 — short press = activity (also handles deep-sleep wake separately).
    //        Long press (≥ 2 s) = toggle WiFi AP + control panel + CTF.
    //
    // We can't block in this function (the rest of the loop has to keep
    // running so the long-press is visually obvious), so we use a small state
    // machine across calls.
    if (BTN2 == GPIO_NUM_2) {
        static unsigned long btn2DownStart = 0;
        static bool          btn2WasDown   = false;
        static bool          btn2LongFired = false;

        bool nowDown = (digitalRead(BTN2) == LOW);
        unsigned long now = millis();

        if (nowDown && !btn2WasDown) {              // press edge
            btn2DownStart = now;
            btn2LongFired = false;
        }
        if (nowDown && !btn2LongFired && (now - btn2DownStart) >= BTN2_LONGPRESS_MS) {
            // Long-press threshold crossed — toggle the portal.
            btn2LongFired = true;
            lastActivityTime = esp_timer_get_time();
            if (wifi_portal_is_active()) {
                ESP_LOGW(TAG, "BTN2 long-press: stopping WiFi portal.");
                wifi_portal_stop();
                // Restore the awake-indicator green on RGB[0].
                RGB_LEDS[0] = CRGB::Green;
                FastLED.show();
            } else {
                ESP_LOGW(TAG, "BTN2 long-press: starting WiFi portal.");
                if (wifi_portal_start()) {
                    // Visual cue: RGB[0] solid BLUE while AP is active.
                    RGB_LEDS[0] = CRGB::Blue;
                    FastLED.show();
                }
            }
        }
        if (!nowDown && btn2WasDown) {              // release edge
            if (!btn2LongFired) {
                // Short press → just register activity.
                lastActivityTime = esp_timer_get_time();
            }
            btn2DownStart = 0;
        }
        btn2WasDown = nowDown;
    }
}


// =============================================================================
// Sensor pollers
// =============================================================================
// Both sensors share the same "engaged for N ms → trigger" pattern with a
// state-machine. The colour shown when triggered is derived from the current
// esp_timer value, giving a pseudo-random colour each time.

static inline CRGB timerDerivedColour()
{
    uint64_t t = esp_timer_get_time();
    return CRGB((t >> 0) & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF);
}

static void checkHallEffectSensor(void)
{
    unsigned long now = millis();

    // Software debounce — stray magnetic fields (laptop hinges, speakers,
    // SSDs) can briefly trip the sensor. We require continuous "active"
    // reading for HALL_DEBOUNCE_MS before declaring the magnet present
    // for the indicator LED and the 2.5-second hold puzzle. SOS Morse
    // detection still uses the raw reading (it has its own debouncer
    // tuned for short tap pulses).
    static unsigned long hallRawActiveStart = 0;
    int  hallRawPin   = digitalRead(HALL);
    bool hallRaw      = (hallRawPin == 0);     // active-LOW: 0 = magnet present
    bool hallDebounced;
    if (hallRaw) {
        if (hallRawActiveStart == 0) hallRawActiveStart = now;
        hallDebounced = (now - hallRawActiveStart >= HALL_DEBOUNCE_MS);
    } else {
        hallRawActiveStart = 0;
        hallDebounced = false;
    }

    // -------------------------------------------------------------------------
    // Verbose periodic debug log — prints every 500 ms so we can see exactly
    // what the sensor is reporting in real time. Useful for diagnosing
    // "stuck active" behaviour (faulty sensor, nearby magnetic field, broken
    // trace pulling the line low, etc.).
    // -------------------------------------------------------------------------
    static unsigned long lastHallDebugMs = 0;
    if (now - lastHallDebugMs >= 500) {
        lastHallDebugMs = now;
        unsigned long activeFor = hallRaw ? (now - hallRawActiveStart) : 0;
        int luxAnalog = analogRead(LUX);
        // ESP_LOGD so it's compile-time silenced at the default INFO log level.
        // Bump CONFIG_LOG_DEFAULT_LEVEL or call esp_log_level_set(TAG, ESP_LOG_DEBUG)
        // at runtime to re-enable when diagnosing the Hall sensor again.
        ESP_LOGD(TAG,
                 "HALL pin=%d raw=%s activeMs=%lu debounced=%s "
                 "state=%d holdMs=%lu | LUX analog=%d",
                 hallRawPin,
                 hallRaw ? "ACTIVE" : "idle",
                 activeFor,
                 hallDebounced ? "YES" : "no",
                 (int)hallState,
                 (hallTsStart && hallDebounced) ? (now - hallTsStart) : 0UL,
                 luxAnalog);
    }

    // HALL_LED follows the sensor's latched state directly.
    //
    // MGH201A1T3 is a bipolar latching Hall switch: one polarity of magnet
    // pulls the OUT line LOW (LED on), the opposite polarity releases it
    // HIGH (LED off). State is held when the magnet is removed. We don't
    // gate this with any "puzzle solved" flag — what you see is what the
    // sensor sees.
    CLED_State[HALL_LED] = hallDebounced ? ON : OFF;

    switch (hallState) {
        case WAIT_HALL_ON1:
            if (hallDebounced) {                                            // sustained engagement
                if (hallTsStart == 0) {
                    hallTsStart = now;
                } else if (now - hallTsStart >= HALL_HOLD_DURATION_MS) {    // held long enough
                    hallState        = TRIGGER_HALL;
                    hallTsStart      = now;
                    lastActivityTime = esp_timer_get_time();
                }
            } else {
                hallTsStart = 0;
            }
            break;

        case TRIGGER_HALL:
            RGB_LEDS[HALL_RGB] = timerDerivedColour();
            FastLED.show();
            hallState = WAIT_HALL_ON1;
            break;
    }
}

static void checkLuxSensor(void)
{
    unsigned long now = millis();

    // Update solved indicator LED based on instantaneous analog reading.
    int reading = analogRead(LUX);
    if (reading < LUX_DARK_THRESHOLD) {
        CLED_State[LUX_LED] = OFF;
    } else if (reading > LUX_LIGHT_THRESHOLD) {
        CLED_State[LUX_LED] = ON;
    }

    switch (luxState) {
        case WAIT_LUX_ON1:
            if (digitalRead(LUX) == 0) {                                  // sensor pin pulled low
                if (luxTsStart == 0) {
                    luxTsStart = now;
                } else if (now - luxTsStart >= LUX_HOLD_DURATION_MS) {
                    luxState         = TRIGGER_LUX;
                    luxTsStart       = now;
                    lastActivityTime = esp_timer_get_time();
                }
            } else {
                luxTsStart = 0;
            }
            break;

        case TRIGGER_LUX:
            RGB_LEDS[LUX_RGB] = timerDerivedColour();
            FastLED.show();
            luxState = WAIT_LUX_ON1;
            break;
    }
}




// =============================================================================
// Master unlock — all 8 presentations AND Malware Village attended
// =============================================================================
// When the user has completed every attendance-based challenge, the otherwise-
// unused RGB slot 3 lights up in the BSides 2025 conference orange. The colour
// is persisted to NVS so it survives a power cycle.
static void checkAndApplyMasterUnlock(void)
{
    // All 8 presentation tracker bits set?
    for (int i = 0; i < 8; i++) {
        if (presentationTracker[i] != 1) return;
    }
    // Malware Village attended?
    if (CLED_State[Malware_Village] != ON && CLED_Save_State[Malware_Village] != ON) {
        return;
    }
    // Already lit? Avoid spurious NVS writes.
    if (RGB_LEDS[3] == MASTER_UNLOCK_COLOUR) {
        return;
    }
    ESP_LOGW(TAG, "Master unlock — all attendance challenges complete!");
    RGB_LEDS[3] = MASTER_UNLOCK_COLOUR;
    FastLED.show();
    BadgeState::saveRgb4();   // RGB4_* keys cover slot 3 (index-3, 0-based)
}
