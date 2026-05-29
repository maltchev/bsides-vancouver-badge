# =============================================================================
# Starter template — BSides Vancouver Badge
# =============================================================================
#
# A blank-canvas ESP-IDF project for the BSides badge. It reuses the same
# vendored components (FastLED, NFC RFAL, ST25R3911B driver, Arduino layer)
# as the main firmware, so attendees only need to focus on the application
# code in main/.
#
# Build with:
#     cd starter-template
#     idf.py set-target esp32c3
#     idf.py build
#     idf.py -p <PORT> flash monitor
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# Reuse the components from the main firmware tree (../components) rather
# than duplicating them here. Same Arduino layer, same FastLED, same NFC
# stack — only `main/` is project-local.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BSides_Badge_Starter)
