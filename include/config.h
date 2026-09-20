#pragma once

#include "driver/gpio.h"

// LILYGO T-Dongle-S3 pin map (based on board silk/pinout image).
//
// Ported off Arduino's pinMode/digitalWrite to driver/gpio.h as part of the
// ESP-IDF migration (PLANO_ESPIDF_DONGLE.md, phase 1). Pin constants are now
// gpio_num_t (not uint8_t) so they pass directly to gpio_set_level()/
// gpio_config() without a cast at every call site -- the couple of Grupo C
// libs that still reference these as plain pinMode() arguments (DonglePeripherals,
// StartupConfig) are not part of this build yet (see the plan's phase 7) and
// get their own pass when they are ported.
namespace BoardConfig {

// Buttons
static constexpr gpio_num_t PIN_BOOT_BUTTON = GPIO_NUM_0;

// USB native pins
static constexpr gpio_num_t PIN_USB_DN = GPIO_NUM_19;
static constexpr gpio_num_t PIN_USB_DP = GPIO_NUM_20;

// UART header pins
static constexpr gpio_num_t PIN_UART_TX = GPIO_NUM_43;  // TX
static constexpr gpio_num_t PIN_UART_RX = GPIO_NUM_44;  // RX

// LCD ST7735 (SPI)
static constexpr gpio_num_t PIN_TFT_CS = GPIO_NUM_4;
static constexpr gpio_num_t PIN_TFT_SDA = GPIO_NUM_3;  // MOSI
static constexpr gpio_num_t PIN_TFT_SCL = GPIO_NUM_5;  // SCLK
static constexpr gpio_num_t PIN_TFT_DC = GPIO_NUM_2;
static constexpr gpio_num_t PIN_TFT_RES = GPIO_NUM_1;
static constexpr gpio_num_t PIN_TFT_LED = GPIO_NUM_38;  // backlight
static constexpr uint8_t TFT_COL_START = 26;             // ST7735 RAM col offset calibration
static constexpr uint8_t TFT_ROW_START = 1;               // ST7735 RAM row offset calibration

// TF card (SDMMC bus on this board)
static constexpr gpio_num_t PIN_SDMMC_D0 = GPIO_NUM_14;
static constexpr gpio_num_t PIN_SDMMC_D1 = GPIO_NUM_17;
static constexpr gpio_num_t PIN_SDMMC_D2 = GPIO_NUM_18;
static constexpr gpio_num_t PIN_SDMMC_D3 = GPIO_NUM_21;
static constexpr gpio_num_t PIN_SDMMC_CLK = GPIO_NUM_12;
static constexpr gpio_num_t PIN_SDMMC_CMD = GPIO_NUM_16;

// Onboard clock/data LED driver lines
static constexpr gpio_num_t PIN_LED_DI = GPIO_NUM_40;
static constexpr gpio_num_t PIN_LED_CI = GPIO_NUM_39;

// Shell "sudo" password (see SudoManager): elevates the calling identity
// (serial console, or one specific ESP-NOW peer) so it can run destructive
// commands. Elevation only lives in RAM and always resets on reboot.
// CHANGE THIS before flashing a device you actually care about.
static constexpr const char* SUDO_PASSWORD = "657585";

namespace detail {

inline void configureOutputs(uint64_t pinBitMask) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = pinBitMask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

inline void configureInputs(uint64_t pinBitMask, bool pullUp) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = pinBitMask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = pullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

}  // namespace detail

// Configure all board GPIO directions and default levels.
// Keep lcdBacklightOn=false for safe startup with screen off.
inline void initBoardPins(bool lcdBacklightOn = false) {
    // Inputs
    detail::configureInputs((1ULL << PIN_BOOT_BUTTON), /*pullUp=*/true);
    detail::configureInputs((1ULL << PIN_UART_RX), /*pullUp=*/false);

    // PIN_USB_DN/PIN_USB_DP (GPIO19/20) are deliberately NOT touched here.
    // Bug found on the bench (ESP-IDF migration, PLANO_ESPIDF_DONGLE.md):
    // gpio_config() sets a pin's IO_MUX selector to plain GPIO function,
    // which disconnects it from whichever dedicated USB controller (the
    // built-in USB-Serial/JTAG bridge used for flashing/console today, or
    // esp_tinyusb's USB-OTG in phase 6) is supposed to own it -- and unlike
    // Arduino's USB.begin()/Serial.begin(), which used to reclaim these
    // pins right after this same pinMode(..., INPUT) call, nothing under
    // ESP-IDF reconfigures them back. The result: the very next boot after
    // this ran, the dongle stopped enumerating as a USB device at all
    // (recovered only by forcing ROM download mode with the BOOT button).
    // Leave these two pins at their power-on-reset default -- that default
    // is already correct for USB, which is also why flashing itself works
    // before app_main() ever runs.

    // Outputs
    detail::configureOutputs(
        (1ULL << PIN_UART_TX) | (1ULL << PIN_TFT_CS) | (1ULL << PIN_TFT_SDA) |
        (1ULL << PIN_TFT_SCL) | (1ULL << PIN_TFT_DC) | (1ULL << PIN_TFT_RES) |
        (1ULL << PIN_TFT_LED) | (1ULL << PIN_LED_DI) | (1ULL << PIN_LED_CI));

    gpio_set_level(PIN_UART_TX, 1);  // UART idle level

    // Safe defaults for SPI LCD lines before display init
    gpio_set_level(PIN_TFT_CS, 1);
    gpio_set_level(PIN_TFT_SDA, 0);
    gpio_set_level(PIN_TFT_SCL, 0);
    gpio_set_level(PIN_TFT_DC, 0);
    gpio_set_level(PIN_TFT_RES, 1);
    gpio_set_level(PIN_TFT_LED, lcdBacklightOn ? 1 : 0);

    // SDMMC lines: keep pulled-up until SD/MMC driver starts
    detail::configureInputs(
        (1ULL << PIN_SDMMC_D0) | (1ULL << PIN_SDMMC_D1) | (1ULL << PIN_SDMMC_D2) |
        (1ULL << PIN_SDMMC_D3) | (1ULL << PIN_SDMMC_CLK) | (1ULL << PIN_SDMMC_CMD),
        /*pullUp=*/true);

    // LED data/clock lines
    gpio_set_level(PIN_LED_DI, 0);
    gpio_set_level(PIN_LED_CI, 0);
}

}  // namespace BoardConfig
