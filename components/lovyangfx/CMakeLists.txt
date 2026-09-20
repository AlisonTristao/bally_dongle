# LovyanGFX as an ESP-IDF component, NOT a PlatformIO lib_deps entry.
#
# Why: under `framework = espidf` PlatformIO compiles every lib_deps library
# with its own LDF-driven builder, OUTSIDE the ESP-IDF CMake component graph
# -- same class of problem as BTP (see components/btp/CMakeLists.txt), but a
# different symptom here. LovyanGFX mixes .cpp and .c sources (the .c ones
# are glyph tables and image-codec helpers under lgfx/utility and
# lgfx/Fonts), and PlatformIO's LDF-driven builder applies the SAME global
# flag set to both: this project's framework passes
# `-fuse-cxa-atexit` (a C++-only flag) through to every file, so the .c ones
# fail with "command-line option '-fuse-cxa-atexit' is valid for C++/ObjC++
# but not for C [-Werror]" the moment lib_deps tries to build them. A real
# ESP-IDF CMake component does not have this problem -- CMake applies
# per-language COMPILE_OPTIONS via generator expressions, so C files never
# see C++-only flags.
#
# Vendored (not a git submodule/FetchContent) at tag 1.2.29, same reasoning
# as components/esp32-idf-sqlite3 -- this project pins dependencies as an
# actual checked-in copy, not a moving upstream ref. Two things were
# deliberately dropped from the upstream tree before vendoring:
#
# - lgfx/Fonts/efont/*.c and lgfx/Fonts/IPA/*.c (the CJK glyph-bitmap data,
#   ~120 MB total upstream) -- lgfx_fonts.cpp #includes their tiny .h
#   declarations unconditionally, but the actual `extern const uint8_t[]`
#   *data* only gets linked in if something references the `fonts::efontXX`/
#   `fonts::IPAfont*` globals that wrap it (see lgfx_fonts.cpp's `fontdata[]`
#   array, which only lists Font0/2/4/6/7/8 -- none of the CJK fonts). This
#   firmware only ever uses the default font, so with
#   `-ffunction-sections -fdata-sections` + `--gc-sections` (ESP-IDF's
#   default) those globals -- and the .c-file data they would have pulled in
#   -- get discarded unreferenced. Their .h files are kept (small, and
#   lgfx_fonts.cpp's #includes need them to compile) so re-adding a .c back
#   here is a pure addition if a future feature actually wants a CJK font.
# - lgfx/v1/platforms/{rp2040,esp32c3,esp32s2,esp32p4,samd21,samd51,esp8266,
#   arduino_default,stm32,spresense,sdl,opencv,framebuffer}/ -- device.hpp's
#   platform auto-detect takes its `#if defined(ESP_PLATFORM)` branch first
#   and, for CONFIG_IDF_TARGET_ESP32S3, only ever includes esp32/ and
#   esp32s3/ headers; every other platform directory is dead weight for this
#   target and was never going to be reached by any #include in this build.
#
# Panel/touch driver .hpp files for chips this board doesn't have (GC9A01,
# ILI9341, XPT2046, ...) ARE kept, because lgfx/v1_init.hpp #includes every
# one of them unconditionally regardless of which panel a project actually
# uses -- LovyanGFX.hpp itself would fail to compile without them present.
# Their .cpp files are compiled too (SRCS glob below): small individually,
# and --gc-sections drops the ones nothing instantiates, same as the font
# globals above.

file(GLOB LOVYANGFX_TOP_SRCS "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/*.cpp")
file(GLOB LOVYANGFX_MISC_SRCS "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/misc/*.cpp")
file(GLOB LOVYANGFX_PANEL_SRCS "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/panel/*.cpp")
file(GLOB LOVYANGFX_TOUCH_SRCS "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/touch/*.cpp")
file(GLOB LOVYANGFX_UTILITY_SRCS "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/utility/*.c")

# Explicit file list here, NOT a blanket glob like the ones above: this
# directory also holds Bus_EPD/Bus_HUB75/Panel_CVBS/Panel_EPD/Light_CH422G,
# each requiring its own extra ESP-IDF component (esp_lcd for Bus_EPD's
# esp_lcd_panel_io.h, ...) that this project has no other reason to pull
# in. device.hpp's own `#elif defined(CONFIG_IDF_TARGET_ESP32S3)` branch
# only ever includes common.hpp + these three headers -- matching that
# exactly instead of compiling everything in the directory.
set(LOVYANGFX_ESP32_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32/common.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32/Bus_SPI.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32/Bus_I2C.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32/Light_PWM.cpp"
)
# Same reasoning: esp32s3/Bus_RGB.cpp + Panel_RGB.cpp are for the parallel
# RGB LCD interface (a different ESP-IDF component, esp_lcd again) that this
# board's SPI-only ST7735 never touches -- device.hpp only includes
# Bus_Parallel8/16 for CONFIG_IDF_TARGET_ESP32S3.
set(LOVYANGFX_ESP32S3_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32s3/Bus_Parallel8.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lovyangfx/lgfx/v1/platforms/esp32s3/Bus_Parallel16.cpp"
)

idf_component_register(
    SRCS
        ${LOVYANGFX_TOP_SRCS}
        ${LOVYANGFX_MISC_SRCS}
        ${LOVYANGFX_PANEL_SRCS}
        ${LOVYANGFX_TOUCH_SRCS}
        ${LOVYANGFX_ESP32_SRCS}
        ${LOVYANGFX_ESP32S3_SRCS}
        ${LOVYANGFX_UTILITY_SRCS}
    INCLUDE_DIRS "lovyangfx"
    # IDF 6's `driver` component is a thin compatibility shim -- the actual
    # driver/*.h headers (gpio.h, spi_master.h, ledc.h, i2c.h) each live in
    # their own esp_driver_* component now. platforms/esp32/*.cpp uses all
    # four (Bus_SPI, Bus_I2C, Light_PWM) even though this project only ever
    # instantiates Bus_SPI -- the others still have to compile since they
    # are part of this component's SRCS glob (see the header comment above).
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_ledc esp_driver_i2c esp_timer esp_hw_support esp_rom efuse
)
