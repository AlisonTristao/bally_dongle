set(COMPONENT_ADD_INCLUDEDIRS "include")

idf_component_register(SRCS "sqlite3.c" "esp32.c" "shox96_0_2.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES mbedtls
                       PRIV_REQUIRES console spiffs)

# ESP-IDF migration (PLANO_ESPIDF_DONGLE.md, phase 5, decisao D2): vendored
# third-party amalgamation, same "upstream warnings don't count" carve-out
# CONTRIBUTING.md already granted the old Sqlite3Esp32 lib_deps entry.
# GCC 15 (this toolchain) promotes -Wimplicit-fallthrough to an error by
# default; the sqlite3.c amalgamation's switch statements predate that
# convention. Not present in the upstream flag list this file shipped with,
# which was tuned against an older/less strict GCC.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu99 -g3 -fno-stack-protector -ffunction-sections -fdata-sections -fstrict-volatile-bitfields -mlongcalls -nostdlib -Wpointer-arith -Wno-error=unused-value -Wno-error=unused-label -Wno-error=unused-function -Wno-error=unused-but-set-variable -Wno-error=unused-variable -Wno-error=deprecated-declarations -Wno-error=char-subscripts -Wno-error=maybe-uninitialized -Wno-error=implicit-fallthrough -Wno-error=stringop-overflow -Wno-error=stringop-truncation -Wno-error=array-bounds -Wno-error=format-truncation -Wno-error=format-overflow -Wno-error=cast-function-type -Wno-error=empty-body -Wno-error=missing-field-initializers -Wno-error=type-limits -Wno-unused-parameter -Wno-unused-variable -Wno-implicit-function-declaration -Wno-sign-compare -Wno-old-style-declaration -MMD -c -DF_CPU=240000000L -DESP32 -DCORE_DEBUG_LEVEL=0 -DNDEBUG)
