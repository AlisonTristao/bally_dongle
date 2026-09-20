#pragma once

#include <cstdint>
#include <string>

#include <LovyanGFX.hpp>

// ESP-IDF migration, phase 7 (PLANO_ESPIDF_DONGLE.md secao 6, D1): the LCD
// stack is rewritten against LovyanGFX (its esp32/Bus_SPI backend talks to
// the IDF SPI driver directly -- no Arduino dependency, auto-selected by
// LovyanGFX.hpp itself off ESP_PLATFORM/CONFIG_IDF_TARGET_ESP32S3) instead
// of Adafruit_GFX/Adafruit_ST7735. Every method below keeps its pre-
// migration name and behaviour; only the panel driver underneath changed.
//
// SD-card methods that used to live on DonglePeripherals (beginSd,
// isSdReady, sdCardTypeName, wipeSdContents, ...) are NOT ported here --
// phase 5 already carved that out into DongleSdCard
// (esp_vfs_fat_sdmmc_mount), so the old SD_MMC-based code this class used
// to carry was dead weight even before this port. Use DongleSdCard instead.

/**
 * @brief Custom LGFX_Device for the LILYGO T-Dongle-S3's onboard 0.96" ST7735
 * (160x80, "R"-variant init sequence -- same chip family the old
 * Adafruit_ST7735::initR() call targeted, see Panel_ST7735S vs. the
 * B-variant Panel_ST7735). No MISO wiring on this board (write-only bus,
 * same as the old software-SPI 4-wire Adafruit constructor), so `readable`
 * stays false.
 */
class DongleLcd final : public lgfx::LGFX_Device {
public:
    DongleLcd();

private:
    lgfx::Panel_ST7735S panel_;
    lgfx::Bus_SPI bus_;
};

/**
 * @brief Unified peripheral manager for LILYGO T-Dongle-S3.
 *
 * Covered peripherals:
 * - onboard RGB LED driver (DI/CI)
 * - ST7735 LCD display
 */
class DonglePeripherals final {
public:
    DonglePeripherals();

    /**
     * @brief Initializes LED and LCD with safe defaults.
     */
    void begin();

    /**
     * @brief Initializes LED interface only.
     */
    bool beginLed();

    /**
     * @brief Sets RGB color for onboard LED.
     * @param r Red channel 0..255.
     * @param g Green channel 0..255.
     * @param b Blue channel 0..255.
     * @param brightness31 Global brightness 0..31.
     */
    void setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness31 = 31);

    /**
     * @brief Turns LED off.
     */
    void ledOff();

    /**
     * @brief Initializes ST7735 LCD.
     * @param rotation Display rotation 0..3.
     */
    bool beginLcd(uint8_t rotation = 1);

    /**
     * @brief Returns true when LCD is initialized.
     */
    bool isLcdReady() const;

    /**
     * @brief Reinitializes LCD and clears the screen.
     */
    bool reinitLcd(uint8_t rotation = 1);

    /**
     * @brief Updates LCD rotation at runtime.
     * @param rotation Orientation index 0..3.
     */
    void setLcdRotation(uint8_t rotation);

    /**
     * @brief Returns current LCD rotation index.
     */
    uint8_t lcdRotation() const;

    /**
     * @brief Returns LCD object pointer, ensuring init when possible.
     */
    DongleLcd* lcd();

    /**
     * @brief Controls LCD backlight pin.
     */
    void setLcdBacklight(bool on);

    /**
     * @brief Sets backlight electrical polarity.
     * @param activeHigh true when HIGH means ON, false when LOW means ON.
     */
    void setLcdBacklightPolarity(bool activeHigh);

    /**
     * @brief Returns current software state of LCD backlight.
     */
    bool isLcdBacklightOn() const;

    /**
     * @brief Returns configured backlight polarity.
     */
    bool isLcdBacklightActiveHigh() const;

    /**
     * @brief Clears screen and writes text at top-left.
     * @param text Text to print.
     * @param clearFirst Clear screen before print.
     * @param color RGB565 text color (0xFFFF = white).
     */
    bool writeLcd(const std::string& text, bool clearFirst = true, uint16_t color = 0xFFFF);

    /**
     * @brief Fills LCD screen with one color.
     */
    bool clearLcd(uint16_t color = 0x0000);

private:
    DongleLcd tft_;
    bool ledReady_;
    bool lcdReady_;
    bool lcdBacklightOn_;
    bool lcdBacklightActiveHigh_;
    uint8_t lcdRotation_;

    void sendLedByte(uint8_t value) const;
    void writeLedFrame(uint8_t brightness31, uint8_t r, uint8_t g, uint8_t b) const;
};
