#include "DongleSdCard.h"

#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include "esp_log.h"

namespace DongleSdCard {
namespace {

const char* kTag = "sdcard";

bool g_ready = false;
bool g_oneBitMode = false;
uint32_t g_frequencyKHz = 0;
sdmmc_card_t* g_card = nullptr;

void unmount() {
    if (g_card != nullptr) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
        g_card = nullptr;
    }
}

// Recursively removes `path` (file or directory), same shape as
// DonglePeripherals.cpp's old removeTree(fs::FS&, const String&) but over
// POSIX dirent/stat instead of Arduino's fs::FS/File.
bool removeTree(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }

    if (!S_ISDIR(info.st_mode)) {
        return std::remove(path.c_str()) == 0;
    }

    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }

    bool ok = true;
    struct dirent* entry = nullptr;
    while (ok && (entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        ok = removeTree(path + "/" + name);
    }
    closedir(dir);

    if (!ok) {
        return false;
    }
    return ::rmdir(path.c_str()) == 0;
}

}  // namespace

bool begin(bool oneBitMode) {
    g_ready = false;
    g_oneBitMode = oneBitMode;
    g_frequencyKHz = 0;

    unmount();

    struct SdAttempt {
        bool mode1bit;
        int frequencyKHz;
    };

    SdAttempt attempts[4] = {};
    std::size_t attemptsCount = 0;

    if (oneBitMode) {
        attempts[0] = {true, SDMMC_FREQ_DEFAULT};
        attempts[1] = {true, SDMMC_FREQ_PROBING};
        attemptsCount = 2;
    } else {
        // Try stable 1-bit first to avoid noisy init failures on weak signal cards.
        attempts[0] = {true, SDMMC_FREQ_DEFAULT};
        attempts[1] = {false, SDMMC_FREQ_DEFAULT};
        attempts[2] = {true, SDMMC_FREQ_PROBING};
        attempts[3] = {false, SDMMC_FREQ_PROBING};
        attemptsCount = 4;
    }

    const esp_vfs_fat_mount_config_t mountConfig = {
        /*format_if_mount_failed=*/true,
        /*max_files=*/5,
        /*allocation_unit_size=*/0,
        /*disk_status_check_enable=*/false,
        /*use_one_fat=*/false,
    };

    for (std::size_t i = 0; i < attemptsCount; ++i) {
        const SdAttempt& attempt = attempts[i];
        unmount();

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = attempt.frequencyKHz;
        if (attempt.mode1bit) {
            host.flags &= ~static_cast<decltype(host.flags)>(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT);
        }

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = BoardConfig::PIN_SDMMC_CLK;
        slot.cmd = BoardConfig::PIN_SDMMC_CMD;
        slot.d0 = BoardConfig::PIN_SDMMC_D0;
        slot.d1 = BoardConfig::PIN_SDMMC_D1;
        slot.d2 = BoardConfig::PIN_SDMMC_D2;
        slot.d3 = BoardConfig::PIN_SDMMC_D3;
        slot.width = attempt.mode1bit ? 1 : 4;
        // Same intent as the old gpio_pullup_en() calls on CMD/D0-D3 before
        // SD_MMC.begin(): weak internal pull-ups help; external ones are
        // still recommended.
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        sdmmc_card_t* card = nullptr;
        const esp_err_t mountResult =
            esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mountConfig, &card);
        if (mountResult != ESP_OK || card == nullptr) {
            continue;
        }

        g_card = card;
        g_ready = true;
        g_oneBitMode = attempt.mode1bit;
        g_frequencyKHz = static_cast<uint32_t>(attempt.frequencyKHz);
        return true;
    }

    ESP_LOGW(kTag, "SD init falhou (tentou 4-bit/1-bit e clock reduzido)");
    return false;
}

bool isReady() { return g_ready; }
bool oneBitMode() { return g_oneBitMode; }
uint32_t frequencyKHz() { return g_frequencyKHz; }

std::string cardTypeName() {
    if (!g_ready || g_card == nullptr) {
        return "NONE";
    }
    if (g_card->is_mmc) {
        return "MMC";
    }
    // Matches ESP-IDF's own convention (sdmmc_card_print_info): CSD
    // structure version 0 is the original (<=2GB) SDSC layout, anything
    // newer describes an SDHC/SDXC card.
    return g_card->csd.csd_ver == 0 ? "SDSC" : "SDHC";
}

uint64_t totalBytes() {
    if (!g_ready) {
        return 0;
    }
    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(kMountPoint, &total, &free) != ESP_OK) {
        return 0;
    }
    return total;
}

uint64_t totalMB() { return totalBytes() / (1024ULL * 1024ULL); }

uint64_t usedBytes() {
    if (!g_ready) {
        return 0;
    }
    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(kMountPoint, &total, &free) != ESP_OK) {
        return 0;
    }
    return total - free;
}

uint64_t usedMB() { return usedBytes() / (1024ULL * 1024ULL); }

bool wipeContents() {
    if (!g_ready) {
        return false;
    }

    DIR* root = opendir(kMountPoint);
    if (root == nullptr) {
        return false;
    }

    bool ok = true;
    struct dirent* entry = nullptr;
    while (ok && (entry = readdir(root)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        ok = removeTree(std::string(kMountPoint) + "/" + name);
    }
    closedir(root);
    return ok;
}

}  // namespace DongleSdCard
