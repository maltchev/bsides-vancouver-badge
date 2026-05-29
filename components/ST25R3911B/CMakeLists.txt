FILE(GLOB library_sources src/*.c src/*.cpp)

set(includedirs
  src
  )

  set(requires arduino-esp32 NFC_RFAL)

idf_component_register(SRCS ${library_sources}
                        INCLUDE_DIRS ${includedirs}
                        REQUIRES ${requires}
                        )