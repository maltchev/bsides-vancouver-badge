// =============================================================================
//  badge_state.h — BSides Vancouver Badge — Persistent state interface
// =============================================================================
//
//  Thin wrapper around the Arduino `Preferences` (ESP-IDF NVS) API for the
//  badge's progress data. Collapses the v1-style "begin / get / set / end"
//  pattern into named operations so call sites stay readable.
//
//  NVS namespace: "badge"
//
// =============================================================================
#pragma once

namespace BadgeState {

// Restore every persisted field into the live globals (RGB_LEDS, CLED_State,
// presentationCounter, presentationTracker, FastLED brightness). Call once at
// boot before doing anything that reads those globals.
void restoreAll();

// Save operations — call after the corresponding state changes.
void saveTeamColours();          // RGB[1] and (if NUM_LEDS==5) RGB[4]
void savePresentationProgress(); // counter + 3 indicator LEDs + 8 tracker bits + RGB[1] + RGB[4]
void saveMalwareFlag();          // Malware Village LED state
void saveHallFlag();             // Hall puzzle solved LED
void saveLuxFlag();              // Lux puzzle solved LED
void saveBrightness();           // global FastLED brightness
void saveRgb1();                 // RGB[0] (HALL_RGB)
void saveRgb3();                 // RGB[2] (LUX_RGB)
void saveRgb4();                 // RGB[3] (reserved / master-unlock indicator)

// First-boot component self-test gating.
bool firstBootCheckComplete();
void markFirstBootCheckComplete();

// Erase every persisted key in the badge namespace. Used by the BADGEWIPE
// NFC tag and by the BTN2-held-at-startup hard reset.
void wipeAll();

}
