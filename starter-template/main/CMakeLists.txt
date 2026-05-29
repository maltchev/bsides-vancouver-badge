idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        # System-level
        nvs_flash driver
        # Vendored components — same as the main firmware's main/CMakeLists.txt
        espressif__arduino-esp32 FastLED NFC_RFAL ST25R3911B
)
