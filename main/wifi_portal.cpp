// =============================================================================
//  wifi_portal.cpp — BSides Vancouver 2025/2026 Badge
// =============================================================================
//
//  WiFi AP + HTTP server.  See wifi_portal.h for the design rationale.
//
//  Endpoints exposed:
//
//      GET  /                 → single-page control panel (HTML/JS)
//      GET  /api/state        → JSON snapshot of HALL/LUX/BTN/RGB/etc.
//      POST /api/color        → { "slot": 0..4, "r": 0..255, "g": ..., "b": ... }
//      POST /api/brightness   → { "value": 0..255 }
//      GET  /ctf              → CTF landing page
//      GET  /ctf/login        → vulnerable login form (HTML)
//      POST /ctf/login        → "authenticates", sets a tamperable cookie
//      GET  /ctf/dashboard    → role-gated page; admin sees the flag
//
//      Captive-portal hooks (Android / iOS / Windows probe URLs):
//      /generate_204, /hotspot-detect.html, /ncsi.txt, /redirect, /success.txt
//        → 302 redirect to /
//
//  All HTML is embedded as C++ raw string literals at the bottom of this
//  file.  Both pages are intentionally small (no external CSS / JS / fonts)
//  so the badge can serve them without a network round trip to anywhere.
//
// =============================================================================

#include "wifi_portal.h"

#include <string.h>
#include <stdio.h>
#include <atomic>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#include "FastLED.h"
#include "project_pins.h"

static const char *TAG = "WifiPortal";

// -----------------------------------------------------------------------------
// Globals shared with main.cpp
// -----------------------------------------------------------------------------
extern CRGB     RGB_LEDS[];
extern uint8_t  CLED_State[];
extern uint8_t  presentationCounter;
extern uint8_t  presentationTracker[8];

// RGB slot indices (kept in sync with the #defines at the top of main.cpp).
//   0  HALL_RGB     — awake indicator + colour painted by the Hall hold puzzle
//   1  Team Colour 1 — set by NFC team-colour tags
//   2  LUX_RGB      — colour painted by the Lux cover puzzle
//   3  Reserved     — master-unlock indicator (BSides orange when all challenges done)
//   4  Team Colour 2 — mirror of slot 1
#define RGB_HALL          0
#define RGB_TEAM1         1
#define RGB_LUX           2
#define RGB_RESERVED      3
#define RGB_TEAM2         4

// Charlieplex LED indices (kept in sync with the #defines at the top of main.cpp).
//   0  D3  Presentation 1 attended
//   1  D4  Presentation 3 attended
//   2  D5  Malware Village attended
//   3  D7  Presentation 2 attended
//   4  D8  Hall sensor LED (mirrors the latched sensor state)
//   5  D9  Lux sensor LED (lights when ambient light is bright)
#define CLED_PRES1        0
#define CLED_PRES3        1
#define CLED_MALWARE      2
#define CLED_PRES2        3
#define CLED_HALL         4
#define CLED_LUX          5

// -----------------------------------------------------------------------------
// Module-private state
// -----------------------------------------------------------------------------
static httpd_handle_t   s_httpd      = nullptr;
static esp_netif_t*     s_ap_netif   = nullptr;
static std::atomic<bool> s_active{false};
static std::atomic<bool> s_flag_captured{false};

// Forward decls
static esp_err_t handler_root(httpd_req_t *req);
static esp_err_t handler_state(httpd_req_t *req);
static esp_err_t handler_color(httpd_req_t *req);
static esp_err_t handler_brightness(httpd_req_t *req);
static esp_err_t handler_ctf_index(httpd_req_t *req);
static esp_err_t handler_ctf_login_get(httpd_req_t *req);
static esp_err_t handler_ctf_login_post(httpd_req_t *req);
static esp_err_t handler_ctf_dashboard(httpd_req_t *req);
static esp_err_t handler_captive_redirect(httpd_req_t *req);

// Embedded HTML (defined at the bottom).
extern const char HTML_PANEL[];
extern const char HTML_CTF_INDEX[];
extern const char HTML_CTF_LOGIN[];
extern const char HTML_CTF_DASHBOARD_GUEST[];
extern const char HTML_CTF_DASHBOARD_ADMIN[];

// -----------------------------------------------------------------------------
// WiFi event handler (only needs to log; the AP "just works" once configured).
// -----------------------------------------------------------------------------
static void wifi_event_handler(void* /*arg*/, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    if (base != WIFI_EVENT) return;
    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* ev = (wifi_event_ap_staconnected_t*)event_data;
            ESP_LOGI(TAG, "Client connected: %02X:%02X:%02X:%02X:%02X:%02X (aid=%d)",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5], ev->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* ev = (wifi_event_ap_stadisconnected_t*)event_data;
            ESP_LOGI(TAG, "Client disconnected: %02X:%02X:%02X:%02X:%02X:%02X",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5]);
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started.");
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped.");
            break;
        default: break;
    }
}

// -----------------------------------------------------------------------------
// HTTP routing
// -----------------------------------------------------------------------------
static void register_handlers(httpd_handle_t h) {
    auto reg = [&](const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
        httpd_uri_t u = {
            .uri = uri, .method = method, .handler = fn, .user_ctx = nullptr,
        };
        httpd_register_uri_handler(h, &u);
    };

    reg("/",                 HTTP_GET,  handler_root);
    reg("/api/state",        HTTP_GET,  handler_state);
    reg("/api/color",        HTTP_POST, handler_color);
    reg("/api/brightness",   HTTP_POST, handler_brightness);

    reg("/ctf",              HTTP_GET,  handler_ctf_index);
    reg("/ctf/login",        HTTP_GET,  handler_ctf_login_get);
    reg("/ctf/login",        HTTP_POST, handler_ctf_login_post);
    reg("/ctf/dashboard",    HTTP_GET,  handler_ctf_dashboard);

    // Common captive-portal detection probes — Android, iOS, Windows, etc.
    reg("/generate_204",          HTTP_GET, handler_captive_redirect);
    reg("/gen_204",               HTTP_GET, handler_captive_redirect);
    reg("/hotspot-detect.html",   HTTP_GET, handler_captive_redirect);
    reg("/library/test/success.html", HTTP_GET, handler_captive_redirect);
    reg("/ncsi.txt",              HTTP_GET, handler_captive_redirect);
    reg("/connecttest.txt",       HTTP_GET, handler_captive_redirect);
    reg("/redirect",              HTTP_GET, handler_captive_redirect);
    reg("/success.txt",           HTTP_GET, handler_captive_redirect);
}

// -----------------------------------------------------------------------------
// /  (control panel)
// -----------------------------------------------------------------------------
static esp_err_t handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_PANEL, HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------------
// /api/state
//
// Returns a JSON snapshot of everything the badge can see — sensors, buttons,
// addressable RGB chain, charlieplexed indicator LEDs, presentation progress,
// and the master-unlock indicator. The control-panel page polls this every
// 500 ms.
// -----------------------------------------------------------------------------
static esp_err_t handler_state(httpd_req_t *req) {
    int hall = digitalRead(HALL);              // 0 = magnet latched / sensor active
    int lux  = analogRead(LUX);
    int btn1 = digitalRead(BTN1);
    int btn2 = digitalRead(BTN2);
    uint8_t br = FastLED.getBrightness();

    // Master unlock = every presentation attended AND Malware Village attended.
    // Mirrors the logic in main.cpp's checkAndApplyMasterUnlock().
    bool masterUnlock = (CLED_State[CLED_MALWARE] != 0);
    for (int i = 0; i < 8 && masterUnlock; i++) {
        if (presentationTracker[i] == 0) masterUnlock = false;
    }

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"v2.4.1\","
        "\"sensors\":{\"hall\":%d,\"lux\":%d,\"btn1\":%d,\"btn2\":%d},"
        "\"brightness\":%u,"
        "\"rgb\":["
            "{\"r\":%u,\"g\":%u,\"b\":%u},"
            "{\"r\":%u,\"g\":%u,\"b\":%u},"
            "{\"r\":%u,\"g\":%u,\"b\":%u},"
            "{\"r\":%u,\"g\":%u,\"b\":%u},"
            "{\"r\":%u,\"g\":%u,\"b\":%u}"
        "],"
        "\"cled\":[%d,%d,%d,%d,%d,%d],"
        "\"progress\":{"
            "\"presentations\":%u,"
            "\"tracker\":[%d,%d,%d,%d,%d,%d,%d,%d],"
            "\"malware\":%s,"
            "\"master_unlock\":%s"
        "}"
        "}",
        hall, lux, btn1, btn2, br,
        RGB_LEDS[0].r, RGB_LEDS[0].g, RGB_LEDS[0].b,
        RGB_LEDS[1].r, RGB_LEDS[1].g, RGB_LEDS[1].b,
        RGB_LEDS[2].r, RGB_LEDS[2].g, RGB_LEDS[2].b,
        RGB_LEDS[3].r, RGB_LEDS[3].g, RGB_LEDS[3].b,
        RGB_LEDS[4].r, RGB_LEDS[4].g, RGB_LEDS[4].b,
        CLED_State[0]!=0, CLED_State[1]!=0, CLED_State[2]!=0,
        CLED_State[3]!=0, CLED_State[4]!=0, CLED_State[5]!=0,
        presentationCounter,
        presentationTracker[0], presentationTracker[1], presentationTracker[2], presentationTracker[3],
        presentationTracker[4], presentationTracker[5], presentationTracker[6], presentationTracker[7],
        CLED_State[CLED_MALWARE] ? "true" : "false",
        masterUnlock ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// Tiny JSON value extractor — looks for `"key":<number>` and returns the int.
// Robust enough for our trivial control-panel JSON; not a real parser.
static int json_int(const char* body, int len, const char* key, int dflt) {
    char needle[32];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0) return dflt;
    for (int i = 0; i + nlen < len; i++) {
        if (memcmp(body + i, needle, nlen) == 0) {
            // skip whitespace, colon, whitespace
            int j = i + nlen;
            while (j < len && (body[j] == ' ' || body[j] == ':')) j++;
            int sign = 1;
            if (j < len && body[j] == '-') { sign = -1; j++; }
            int v = 0;
            bool got = false;
            while (j < len && body[j] >= '0' && body[j] <= '9') {
                v = v*10 + (body[j]-'0'); j++; got = true;
            }
            return got ? sign*v : dflt;
        }
    }
    return dflt;
}

// -----------------------------------------------------------------------------
// POST /api/color  body = {"slot":N,"r":R,"g":G,"b":B}
// -----------------------------------------------------------------------------
static esp_err_t handler_color(httpd_req_t *req) {
    char buf[256];
    int len = req->content_len < (int)sizeof(buf)-1 ? req->content_len : (int)sizeof(buf)-1;
    int got = httpd_req_recv(req, buf, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    buf[got] = 0;

    int slot = json_int(buf, got, "slot", -1);
    int r    = json_int(buf, got, "r", 0);
    int g    = json_int(buf, got, "g", 0);
    int b    = json_int(buf, got, "b", 0);

    if (slot < 0 || slot >= NUM_LEDS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot out of range");
        return ESP_OK;
    }
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    RGB_LEDS[slot] = CRGB(clamp(r), clamp(g), clamp(b));
    // Don't call FastLED.show() here — the loop() in main.cpp calls it.
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// POST /api/brightness  body = {"value":N}
// -----------------------------------------------------------------------------
static esp_err_t handler_brightness(httpd_req_t *req) {
    char buf[64];
    int len = req->content_len < (int)sizeof(buf)-1 ? req->content_len : (int)sizeof(buf)-1;
    int got = httpd_req_recv(req, buf, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    buf[got] = 0;
    int v = json_int(buf, got, "value", -1);
    if (v < 0 || v > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value 0..255");
        return ESP_OK;
    }
    FastLED.setBrightness((uint8_t)v);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// CTF mini-challenge
// -----------------------------------------------------------------------------
// The bug: /ctf/login sets a Cookie like  `auth=guest`  for the guest creds.
// The dashboard at /ctf/dashboard reads that cookie and returns the "flag"
// page only if `auth=admin`. Browser dev-tools (or curl) can edit the cookie
// trivially → horizontal privilege escalation.
//
// Solving it (i.e. fetching /ctf/dashboard with auth=admin) sets a flag flag
// that main.cpp's loop picks up to light the celebration LEDs.
// -----------------------------------------------------------------------------
static esp_err_t handler_ctf_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_CTF_INDEX, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_ctf_login_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_CTF_LOGIN, HTTPD_RESP_USE_STRLEN);
}

static bool body_contains(const char* body, int len, const char* needle) {
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= len; i++) {
        if (memcmp(body + i, needle, nl) == 0) return true;
    }
    return false;
}

static esp_err_t handler_ctf_login_post(httpd_req_t *req) {
    char buf[256];
    int len = req->content_len < (int)sizeof(buf)-1 ? req->content_len : (int)sizeof(buf)-1;
    int got = httpd_req_recv(req, buf, len);
    if (got <= 0) got = 0;
    buf[got] = 0;

    // Accept any non-empty username + password=guest. Only "guest" creds work
    // through the form — admin creds are deliberately not exposed.
    bool ok = body_contains(buf, got, "username=") && body_contains(buf, got, "password=guest");

    if (ok) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Set-Cookie", "auth=guest; Path=/");
        httpd_resp_set_hdr(req, "Location",   "/ctf/dashboard");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<h2>Login failed</h2><p>Try <code>guest / guest</code>.</p>"
            "<p><a href=\"/ctf/login\">Back</a></p>");
    }
    return ESP_OK;
}

static esp_err_t handler_ctf_dashboard(httpd_req_t *req) {
    // Pull the Cookie header. ESP-IDF's httpd doesn't have first-class cookie
    // parsing but it'll hand us the raw header.
    char cookie[128] = {0};
    size_t clen = sizeof(cookie);
    httpd_req_get_hdr_value_str(req, "Cookie", cookie, clen);

    bool is_admin = strstr(cookie, "auth=admin") != nullptr;
    bool is_guest = strstr(cookie, "auth=guest") != nullptr;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (is_admin) {
        s_flag_captured.store(true);   // main.cpp picks this up and celebrates
        ESP_LOGW(TAG, "*** CTF FLAG CAPTURED — admin cookie supplied ***");
        return httpd_resp_send(req, HTML_CTF_DASHBOARD_ADMIN, HTTPD_RESP_USE_STRLEN);
    }
    if (is_guest) {
        return httpd_resp_send(req, HTML_CTF_DASHBOARD_GUEST, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/ctf/login");
    return httpd_resp_send(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// Captive-portal redirect for the various OS probe URLs.
// -----------------------------------------------------------------------------
static esp_err_t handler_captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// Public API — start / stop
// -----------------------------------------------------------------------------
bool wifi_portal_start(void) {
    if (s_active.load()) return true;

    // Make sure NVS is up (WiFi calibration data lives there). The badge has
    // already initialized NVS via the Preferences API earlier, so this is
    // effectively a no-op but cheap to guard.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return false;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr);

    // SSID = "BSidesBadge-" + last 4 hex chars of MAC.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "BSidesBadge-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len       = strlen(ssid);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;     // workshop-friendly open AP

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return false;
    }

    ESP_LOGW(TAG, "============================================================");
    ESP_LOGW(TAG, "  WiFi AP up.  SSID: %s  (open)", ssid);
    ESP_LOGW(TAG, "  Connect, then visit http://192.168.4.1/");
    ESP_LOGW(TAG, "  CTF challenge: http://192.168.4.1/ctf");
    ESP_LOGW(TAG, "============================================================");

    // HTTP server.
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 16;
    hcfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    register_handlers(s_httpd);

    s_active.store(true);
    return true;
}

void wifi_portal_stop(void) {
    if (!s_active.load()) return;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
    s_active.store(false);
    ESP_LOGW(TAG, "WiFi AP + HTTP server shut down.");
}

bool wifi_portal_is_active(void) {
    return s_active.load();
}

bool wifi_portal_consume_flag(void) {
    return s_flag_captured.exchange(false);
}

// =============================================================================
//  Embedded HTML — kept at the bottom so the C++ above stays readable.
// =============================================================================

// ---- Control panel ---------------------------------------------------------
const char HTML_PANEL[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BSides Badge</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 560px; margin: 0 auto; padding: 1em;
         background:#101418; color:#dde; }
  h1 { font-size: 1.3em; margin: .2em 0; }
  h3 { margin: 0 0 .5em; }
  .muted { color:#888; font-size:.85em; }
  .card { background:#1a1f25; padding: 1em; border-radius: 8px; margin: 1em 0;
          border:1px solid #2a313a; }
  .grid { display:grid; grid-template-columns: max-content 1fr; gap: .35em 1em;
          font-size:.95em; align-items:center; }
  .grid .val { font-family: ui-monospace, monospace; color:#cdf; }
  .led-row { display:flex; align-items:center; gap:.6em; margin:.35em 0; font-size:.95em; }
  .swatch { width:30px; height:30px; border-radius:50%; border:2px solid #333; flex-shrink:0; }
  .dot { width:14px; height:14px; border-radius:50%; border:1px solid #444; flex-shrink:0; }
  .dot.on { background:#ffb84d; box-shadow:0 0 6px #ffb84d; border-color:#ffb84d; }
  .dot.off { background:#222; }
  .lbl { flex:1; }
  .desg { color:#888; font-family: ui-monospace, monospace; font-size:.85em;
          min-width: 2.2em; text-align:right; }
  input[type=color]{ width:50px; height:34px; background:transparent; border:none; }
  input[type=range]{ width:100%; }
  a, a:visited { color:#7af; }
  .pill { display:inline-block; padding:.1em .6em; border-radius:99px;
          background:#243; color:#9f9; font-size:.85em; }
  .progress { display:flex; gap:.3em; margin-top:.4em; flex-wrap:wrap; }
  .progress span { width:22px; height:22px; border-radius:4px; display:flex;
                   align-items:center; justify-content:center; font-size:.75em;
                   border:1px solid #444; color:#888; font-family:ui-monospace,monospace; }
  .progress span.done { background:#264; color:#9f9; border-color:#4a8; }
  .master.unlocked { color:#ffb84d; font-weight:600; }
  .master.locked { color:#888; }
</style></head><body>
<h1>BSides Badge — Live</h1>
<div class="muted">version <span id=ver>?</span> · <span id=clients class=pill>connected</span></div>

<div class="card">
  <h3>Sensors &amp; buttons</h3>
  <div class="grid">
    <div>Hall sensor</div><div class=val id=hall>?</div>
    <div>Lux sensor</div><div class=val id=lux>?</div>
    <div>BTN1 (NFC scan)</div><div class=val id=btn1>?</div>
    <div>BTN2 (activity / WiFi)</div><div class=val id=btn2>?</div>
  </div>
</div>

<div class="card">
  <h3>Addressable RGB LEDs</h3>
  <div id=leds></div>
</div>

<div class="card">
  <h3>Indicator LEDs</h3>
  <div class=muted style="margin-bottom:.5em">Charlieplexed monochrome LEDs on the front of the badge.</div>
  <div id=cleds></div>
</div>

<div class="card">
  <h3>Game progress</h3>
  <div class=grid>
    <div>Presentations</div><div class=val><span id=presCnt>?</span> / 8</div>
    <div>Malware Village</div><div class=val id=mw>?</div>
    <div>Master unlock</div><div class=val id=master>?</div>
  </div>
  <div class=progress id=progress></div>
</div>

<div class="card">
  <h3>Brightness</h3>
  <input type=range id=br min=0 max=255 value=10 oninput="setBr(this.value)">
  <div class=muted><span id=brv>10</span> / 255</div>
</div>

<div class="card">
  <h3><a href="/ctf">→ Try the CTF challenge</a></h3>
  <div class=muted>Captive-portal mini-CTF. Find the flag.</div>
</div>

<script>
// RGB chain labels — match the order of pixels on the badge.
const rgbLabels = [
  { name: "HALL_RGB / Awake",      hint: "Slot 0 — green when idle, blue when WiFi on, Hall puzzle paints it" },
  { name: "Team Colour 1",         hint: "Slot 1 — set by NFC team-colour tags" },
  { name: "LUX_RGB",               hint: "Slot 2 — Lux cover puzzle paints it" },
  { name: "Master Unlock",         hint: "Slot 3 — BSides orange once all challenges done" },
  { name: "Team Colour 2",         hint: "Slot 4 — mirrors Team Colour 1" },
];

// Charlieplex LED labels — match the order of indices in main.cpp.
const cledLabels = [
  { desg: "D3", name: "Presentation 1" },
  { desg: "D4", name: "Presentation 3" },
  { desg: "D5", name: "Malware Village" },
  { desg: "D7", name: "Presentation 2" },
  { desg: "D8", name: "Hall sensor LED" },
  { desg: "D9", name: "Lux sensor LED" },
];

function buildRgbRows() {
  rgbLabels.forEach((info, i) => {
    const row = document.createElement('div'); row.className = 'led-row';
    row.title = info.hint;
    row.innerHTML = `<div class=swatch id=sw${i}></div>
      <div class=lbl>${info.name}</div>
      <input type=color id=cp${i} onchange="setColor(${i},this.value)">`;
    leds.appendChild(row);
  });
}
function buildCledRows() {
  cledLabels.forEach((info, i) => {
    const row = document.createElement('div'); row.className = 'led-row';
    row.innerHTML = `<div class="dot off" id=cd${i}></div>
      <div class=desg>${info.desg}</div>
      <div class=lbl>${info.name}</div>
      <div class=val id=cv${i}>off</div>`;
    cleds.appendChild(row);
  });
}
function buildProgress() {
  for (let i = 0; i < 8; i++) {
    const s = document.createElement('span'); s.id = 'pt'+i; s.textContent = (i+1);
    progress.appendChild(s);
  }
}

async function poll() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    ver.textContent = s.version;

    // Sensors
    hall.textContent = s.sensors.hall === 0 ? 'magnet present (line LOW)' : 'idle (line HIGH)';
    lux.textContent  = `${s.sensors.lux} (analog)`;
    btn1.textContent = s.sensors.btn1 === 0 ? 'pressed' : 'released';
    btn2.textContent = s.sensors.btn2 === 0 ? 'pressed' : 'released';

    // Brightness
    br.value = s.brightness; brv.textContent = s.brightness;

    // RGB chain
    s.rgb.forEach((c, i) => {
      const hex = '#' + [c.r,c.g,c.b].map(x => x.toString(16).padStart(2,'0')).join('');
      document.getElementById('sw'+i).style.background = hex;
      document.getElementById('cp'+i).value = hex;
    });

    // Charlieplex indicator LEDs
    s.cled.forEach((on, i) => {
      const dot = document.getElementById('cd'+i);
      const val = document.getElementById('cv'+i);
      dot.className = on ? 'dot on' : 'dot off';
      val.textContent = on ? 'on' : 'off';
    });

    // Game progress
    presCnt.textContent = s.progress.presentations;
    mw.textContent      = s.progress.malware ? 'attended' : 'not yet';
    master.textContent  = s.progress.master_unlock ? 'UNLOCKED' : 'locked';
    master.className    = 'val master ' + (s.progress.master_unlock ? 'unlocked' : 'locked');
    s.progress.tracker.forEach((t, i) => {
      document.getElementById('pt'+i).className = t ? 'done' : '';
    });

    clients.textContent = 'connected';
    clients.style.background = '#243';
  } catch (e) {
    clients.textContent = 'disconnected';
    clients.style.background = '#422';
  }
}
function setColor(slot, hex) {
  const r = parseInt(hex.substr(1,2),16),
        g = parseInt(hex.substr(3,2),16),
        b = parseInt(hex.substr(5,2),16);
  fetch('/api/color', { method:'POST',
    body: JSON.stringify({slot, r, g, b}) });
}
function setBr(v) {
  brv.textContent = v;
  fetch('/api/brightness', { method:'POST',
    body: JSON.stringify({value: parseInt(v)}) });
}
buildRgbRows(); buildCledRows(); buildProgress();
setInterval(poll, 500); poll();
</script></body></html>
)HTML";

// ---- CTF landing -----------------------------------------------------------
const char HTML_CTF_INDEX[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BSides Badge — CTF</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:540px;margin:0 auto;padding:1em;background:#0c0f12;color:#cfd;}
 h1{color:#7af}
 .ascii{font-family:ui-monospace,monospace;white-space:pre;color:#5fa;font-size:.85em;line-height:1.15;}
 a{color:#9cf}
 code{background:#1a2028;padding:.1em .4em;border-radius:3px;}
</style></head><body>
<pre class=ascii>
  __       _____  _     _
 |  |__   |  _  || |___| |___ ___
 |  _ _|  |     ||     | -_| -_|
 |____/   |__|__||_|_|_|___|___|

  badge mini-CTF
</pre>
<h1>Welcome, attendee.</h1>
<p>This badge is currently broadcasting a small intentionally-vulnerable web app.
   Your job: find the flag.</p>
<p>The challenge is a <b>cookie tampering</b> exercise. The login form accepts
   <code>guest / guest</code>. The dashboard knows about an <i>admin</i> role.</p>
<p>If you log in normally you'll get the guest dashboard. Can you escalate?</p>
<p><a href="/ctf/login">→ Go to login</a></p>
<p class=muted style="color:#666;font-size:.9em">
   Hint: browsers store cookies. They can be edited.</p>
<p><a href="/">← back to badge panel</a></p>
</body></html>
)HTML";

// ---- CTF login -------------------------------------------------------------
const char HTML_CTF_LOGIN[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:380px;margin:3em auto;padding:1em;
      background:#0c0f12;color:#cfd;}
 input{display:block;width:100%;padding:.5em;margin:.3em 0 .8em;font-size:1em;
       border:1px solid #444;background:#181d22;color:#cfd;border-radius:4px;}
 button{padding:.6em 1.2em;background:#247;color:#fff;border:none;border-radius:4px;
        font-size:1em;cursor:pointer;}
 a{color:#9cf}
</style></head><body>
<h2>Login</h2>
<form method=post action=/ctf/login>
 <label>Username<input name=username placeholder="guest"></label>
 <label>Password<input name=password type=password placeholder="guest"></label>
 <button type=submit>Sign in</button>
</form>
<!-- TODO: hash passwords on the server side before next workshop -->
<!-- known roles: guest, admin -->
<p><a href="/ctf">← back</a></p>
</body></html>
)HTML";

// ---- CTF dashboard, guest view (default) ----------------------------------
const char HTML_CTF_DASHBOARD_GUEST[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dashboard — guest</title>
<style>body{font-family:system-ui,sans-serif;max-width:540px;margin:1.5em auto;
            padding:1em;background:#0c0f12;color:#cfd;}
 a{color:#9cf} code{background:#1a2028;padding:.1em .4em;border-radius:3px;}
</style></head><body>
<h2>Welcome, guest</h2>
<p>You are logged in as <b>guest</b>. The admin dashboard is hidden from you.</p>
<p>Open your browser's dev tools, find the <code>auth</code> cookie, and see
   what happens if you change its value.</p>
<p><a href="/ctf/dashboard">Reload dashboard</a> ·
   <a href="/ctf">← challenge home</a></p>
</body></html>
)HTML";

// ---- CTF dashboard, admin view (the win condition) -------------------------
const char HTML_CTF_DASHBOARD_ADMIN[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FLAG CAPTURED</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:540px;margin:1.5em auto;padding:1em;
      background:#001100;color:#9f9;}
 .flag{background:#020;border:2px solid #6f6;padding:1em;border-radius:8px;
       font-family:ui-monospace,monospace;font-size:1.2em;text-align:center;}
 a{color:#9fc}
</style></head><body>
<h1>✓ Flag captured</h1>
<p>You modified your <code>auth</code> cookie to <code>admin</code> and accessed
   a higher-privilege page. This is a textbook <b>horizontal privilege
   escalation</b> bug — the server trusted client-controlled state.</p>
<div class=flag>BSIDES{cookies_are_not_credentials}</div>
<p>Check your badge — the LEDs are celebrating. 🎉</p>
<p><a href="/ctf">← challenge home</a></p>
</body></html>
)HTML";
