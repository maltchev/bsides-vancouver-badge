// =============================================================================
//  badge_state.cpp — BSides Vancouver 2025 Badge — Persistent state
// =============================================================================
//
//  Implementation notes:
//
//   - We keep the original NVS key names byte-for-byte (`RGB1_red`, `presLED1`,
//     `presTrack1`, etc.) so a badge that was previously running v1 will
//     restore its saved state correctly after being re-flashed with v2.
//
//   - Every save function opens the "badge" namespace, writes its fields,
//     then closes the namespace. This matches the v1 pattern. NVS commit
//     happens on close(), so we are tolerant of a power loss between writes
//     of different functions (but not within one function).
//
//   - The live state these functions read from / write to lives in main.cpp
//     as globals. We pull them in via `extern` to avoid a major architectural
//     change.
//
// =============================================================================

#include "badge_state.h"
#include "project_pins.h"
#include "Preferences.h"
#include "FastLED.h"

// -----------------------------------------------------------------------------
// Live state owned by main.cpp
// -----------------------------------------------------------------------------
extern uint8_t  CLED_State[];
extern uint8_t  CLED_Save_State[];
extern CRGB     RGB_LEDS[];
extern CRGB     RGB_LEDS_Save_State[];
extern uint8_t  presentationCounter;
extern uint8_t  presentationTracker[8];

// LED-index aliases — these must stay in sync with the #defines at the top of
// main.cpp. They describe slot positions in CLED_State[] and RGB_LEDS[].
#define Presentation_1    0
#define Presentation_3    1
#define Malware_Village   2
#define Presentation_2    3
#define HALL_LED          4
#define LUX_LED           5

#define HALL_RGB          0
#define Team_Colour1      1
#define LUX_RGB           2
#define Team_Colour2      4

namespace {
    // Module-private Preferences handle. Each function opens it on entry and
    // closes on exit; we never leave it open across calls.
    Preferences nvs;
    constexpr const char* NS = "badge";

    // Small helpers to reduce the verbosity of the per-channel RGB save.
    inline void saveRgbSlot(const char* keyR, const char* keyG, const char* keyB,
                            const CRGB& c) {
        nvs.putUInt(keyR, c.red);
        nvs.putUInt(keyG, c.green);
        nvs.putUInt(keyB, c.blue);
    }
    inline void loadRgbSlot(const char* keyR, const char* keyG, const char* keyB,
                            CRGB& c) {
        c.red   = nvs.getUInt(keyR, 0);
        c.green = nvs.getUInt(keyG, 0);
        c.blue  = nvs.getUInt(keyB, 0);
    }
}

// -----------------------------------------------------------------------------
// First-boot self-test gating
// -----------------------------------------------------------------------------
bool BadgeState::firstBootCheckComplete() {
    nvs.begin(NS, false);
    bool done = (nvs.getBool("initialize", 0) == 1);
    nvs.end();
    return done;
}

void BadgeState::markFirstBootCheckComplete() {
    nvs.begin(NS, false);
    nvs.putBool("initialize", 1);
    nvs.end();
}

// -----------------------------------------------------------------------------
// Total wipe — erase every key in the badge namespace.
// On next boot, restoreAll() will read defaults (zeros) and componentCheck()
// will run again (because `initialize` no longer exists).
// -----------------------------------------------------------------------------
void BadgeState::wipeAll() {
    nvs.begin(NS, false);
    nvs.clear();
    nvs.end();
}

// -----------------------------------------------------------------------------
// Restore everything at boot
// -----------------------------------------------------------------------------
void BadgeState::restoreAll() {
    nvs.begin(NS, false);

    presentationCounter         = nvs.getUInt("presCnt", 0);
    CLED_State[Presentation_1]  = nvs.getBool("presLED1", 0);
    CLED_State[Presentation_2]  = nvs.getBool("presLED2", 0);
    CLED_State[Presentation_3]  = nvs.getBool("presLED3", 0);
    CLED_State[Malware_Village] = nvs.getBool("malwareLED", 0);
    CLED_State[HALL_LED]        = nvs.getBool("hallLED", 0);
    CLED_State[LUX_LED]         = nvs.getBool("luxLED", 0);

    loadRgbSlot("RGB1_red", "RGB1_green", "RGB1_blue", RGB_LEDS[0]);
    loadRgbSlot("RGB2_red", "RGB2_green", "RGB2_blue", RGB_LEDS[1]);
    loadRgbSlot("RGB3_red", "RGB3_green", "RGB3_blue", RGB_LEDS[2]);
    if (NUM_LEDS == 5) {
        loadRgbSlot("RGB4_red", "RGB4_green", "RGB4_blue", RGB_LEDS[3]);
        loadRgbSlot("RGB5_red", "RGB5_green", "RGB5_blue", RGB_LEDS[4]);
    }

    // Presentation tracker bits — 8 separate keys for backward compatibility.
    static const char* TRACKER_KEYS[8] = {
        "presTrack1", "presTrack2", "presTrack3", "presTrack4",
        "presTrack5", "presTrack6", "presTrack7", "presTrack8"
    };
    for (int i = 0; i < 8; i++) {
        presentationTracker[i] = nvs.getBool(TRACKER_KEYS[i], 0);
    }

    // Restore global brightness (paired with saveBrightness() — fixed in
    // v2.4 where the original code persisted brightness but never read it back).
    // 0 from NVS means "no value stored" → keep whatever brightness the caller
    // set before calling restoreAll().
    uint32_t br = nvs.getUInt("brightness", 0);
    if (br > 0 && br <= 255) {
        FastLED.setBrightness((uint8_t)br);
    }

    FastLED.show();
    nvs.end();
}

// -----------------------------------------------------------------------------
// Save operations
// -----------------------------------------------------------------------------
void BadgeState::saveTeamColours() {
    nvs.begin(NS, false);
    saveRgbSlot("RGB2_red", "RGB2_green", "RGB2_blue", RGB_LEDS_Save_State[Team_Colour1]);
    if (NUM_LEDS == 5) {
        saveRgbSlot("RGB5_red", "RGB5_green", "RGB5_blue", RGB_LEDS_Save_State[Team_Colour2]);
    }
    nvs.end();
}

void BadgeState::savePresentationProgress() {
    nvs.begin(NS, false);

    nvs.putUInt("presCnt", presentationCounter);
    nvs.putBool("presLED1", CLED_State[Presentation_1]);
    nvs.putBool("presLED2", CLED_State[Presentation_2]);
    nvs.putBool("presLED3", CLED_State[Presentation_3]);

    saveRgbSlot("RGB2_red", "RGB2_green", "RGB2_blue", RGB_LEDS[Team_Colour1]);
    if (NUM_LEDS == 5) {
        saveRgbSlot("RGB5_red", "RGB5_green", "RGB5_blue", RGB_LEDS[Team_Colour2]);
    }

    static const char* TRACKER_KEYS[8] = {
        "presTrack1", "presTrack2", "presTrack3", "presTrack4",
        "presTrack5", "presTrack6", "presTrack7", "presTrack8"
    };
    for (int i = 0; i < 8; i++) {
        nvs.putBool(TRACKER_KEYS[i], presentationTracker[i]);
    }

    nvs.end();
}

void BadgeState::saveMalwareFlag() {
    nvs.begin(NS, false);
    nvs.putBool("malwareLED", CLED_Save_State[Malware_Village]);
    nvs.end();
}

void BadgeState::saveHallFlag() {
    nvs.begin(NS, false);
    nvs.putBool("hallLED", CLED_State[HALL_LED]);
    nvs.end();
}

void BadgeState::saveLuxFlag() {
    nvs.begin(NS, false);
    nvs.putBool("luxLED", CLED_State[LUX_LED]);
    nvs.end();
}

void BadgeState::saveBrightness() {
    nvs.begin(NS, false);
    nvs.putUInt("brightness", FastLED.getBrightness());
    nvs.end();
}

void BadgeState::saveRgb1() {
    nvs.begin(NS, false);
    saveRgbSlot("RGB1_red", "RGB1_green", "RGB1_blue", RGB_LEDS[HALL_RGB]);
    nvs.end();
}

void BadgeState::saveRgb3() {
    nvs.begin(NS, false);
    saveRgbSlot("RGB3_red", "RGB3_green", "RGB3_blue", RGB_LEDS[LUX_RGB]);
    nvs.end();
}

void BadgeState::saveRgb4() {
    nvs.begin(NS, false);
    saveRgbSlot("RGB4_red", "RGB4_green", "RGB4_blue", RGB_LEDS[3]);
    nvs.end();
}
