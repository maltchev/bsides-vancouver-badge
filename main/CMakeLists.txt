idf_component_register(
    SRCS "main.cpp"
         "badge_state.cpp"
         "wifi_portal.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        # System-level
        esp_wifi esp_netif esp_event esp_http_server nvs_flash
        # Project components (must be explicit once any PRIV_REQUIRES is given).
        # Note: the Arduino-ESP32 component name from the IDF Component Manager
        # is "espressif__arduino-esp32" (double underscore in the registered name).
        espressif__arduino-esp32 FastLED NFC_RFAL ST25R3911B
)
