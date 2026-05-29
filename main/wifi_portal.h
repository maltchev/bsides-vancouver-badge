// =============================================================================
//  wifi_portal.h — BSides Vancouver 2025/2026 Badge
// =============================================================================
//
//  Optional WiFi access point + HTTP server. Two reasons it exists:
//
//   1) Live LED control panel — connect from your phone to a single-page web
//      UI that shows live sensor state and lets you change colours / brightness.
//   2) Captive-portal mini CTF — a deliberately-vulnerable cookie-auth login
//      that demonstrates a "horizontal privilege escalation" web bug. Solving
//      it triggers a celebration on the LEDs.
//
//  Both features live behind a single switch (BTN2 long-press, 2 s). WiFi
//  draws ~80–120 mA active, so it cannot run on coin cells — only enable
//  while plugged into USB power.
//
// =============================================================================
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring the AP + HTTP server up. SSID is "BSidesBadge-XXXX" (last 4 hex of
// the chip MAC); open network on the 2.4 GHz band, channel 1, max 4 clients.
// Returns true on success.
bool wifi_portal_start(void);

// Tear everything down: stop httpd, stop WiFi, free netif. After this returns
// the chip is back to normal (no radio activity).
void wifi_portal_stop(void);

// Is the portal currently up?
bool wifi_portal_is_active(void);

// CTF flag-captured indicator — set by the HTTP handler when a client
// successfully escalates to admin. main.cpp polls this on each loop iteration
// so we can light the appropriate LEDs from the foreground task (calling
// FastLED.show() from the httpd task can race the RMT driver).
bool wifi_portal_consume_flag(void);

#ifdef __cplusplus
}
#endif
