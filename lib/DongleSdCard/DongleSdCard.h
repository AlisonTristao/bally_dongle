#pragma once

// ESP-IDF migration, phase 5 (see PLANO_ESPIDF_DONGLE.md): SD-only extract
// out of DonglePeripherals. DonglePeripherals itself stays Arduino/Adafruit-
// coupled (Adafruit_GFX.h/Adafruit_ST7735.h, not portable until phase 7's
// LovyanGFX switch, decisao D1) and its LCD+LED+SD trio can't be compiled as
// one unit under espidf today -- but the plan's own audit already found the
// SD-only methods share no state with LCD/LED beyond the constructor, so
// this is a real split, not a stub: same behaviour (mount point, pin
// fallback table, 4-bit->1-bit + reduced-clock retries) as
// DonglePeripherals::beginSd() used to have, ported off SD_MMC to
// esp_vfs_fat_sdmmc_mount(). DatabaseStore depends on this, not on
// DonglePeripherals, from phase 5 onward.

#include <cstddef>
#include <cstdint>
#include <string>

namespace DongleSdCard {

// Same mount point Arduino's SD_MMC.begin("/sdcard", ...) used -- kept
// identical so an already-provisioned card's directory layout does not
// change. DatabaseStore's kSqliteDatabasePath is built against this.
constexpr const char* kMountPoint = "/sdcard";

/**
 * @brief Mounts the TF card over the SDMMC bus, matching
 * DonglePeripherals::beginSd()'s old fallback table: 1-bit before 4-bit
 * (stable-first), then a reduced ("probing") clock for both, format-on-
 * mount-failure enabled. oneBitMode=true skips straight to the two 1-bit
 * attempts.
 */
bool begin(bool oneBitMode = false);

bool isReady();
bool oneBitMode();
uint32_t frequencyKHz();

/** "MMC" / "SDSC" / "SDHC" / "UNKNOWN" / "NONE" (not ready). */
std::string cardTypeName();

uint64_t totalBytes();
uint64_t totalMB();
uint64_t usedBytes();
uint64_t usedMB();

/** Recursively removes every file/folder under the mount point's root. */
bool wipeContents();

}  // namespace DongleSdCard
