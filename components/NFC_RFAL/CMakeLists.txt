FILE(GLOB library_sources src/*.c src/*.cpp)

set(includedirs
  src
  )

# Critical: make main/'s ndef_custom_config.h and rfal_custom_config.h
# visible to the NFC_RFAL library files. Without this, the library falls
# back to its own ndef_default_config.h / rfal_default_config.h, which
# define different feature flags (e.g. LISTEN_MODE, ISO_DEP_LISTEN,
# ISO_DEP_APDU_MAX_LEN). Those flags gate conditional members in
# RfalNfcClass / ndefContext, causing the class size and offsets to differ
# between main.cpp (which sees custom config) and the library (which would
# otherwise see defaults). Result: classic ODR-violation crash —
# constructors write at one offset, the library reads at another, fields
# come back NULL/garbage, NDEF poller crashes with a Load access fault.
set(priv_includedirs
  ${CMAKE_CURRENT_LIST_DIR}/../../main
  )

set(requires arduino-esp32)

idf_component_register(SRCS ${library_sources}
                        INCLUDE_DIRS ${includedirs}
                        PRIV_INCLUDE_DIRS ${priv_includedirs}
                        REQUIRES ${requires}
                        )
