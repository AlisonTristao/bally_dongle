#include "DonglePeripherals.h"

#include <driver/gpio.h>

#include "compat.h"
#include "config.h"

DongleLcd::DongleLcd() {
    {
        auto cfg = bus_.config();
        cfg.spi_host = SPI2_HOST;
        cfg.spi_mode = 0;
        cfg.freq_write = 27000000;
        cfg.freq_read = 14000000;
        // Write-only bus (no MISO on this board, same as the old software-
        // SPI 4-wire Adafruit constructor) -- spi_3wire is for half-duplex
        // reads over MOSI, not needed when nothing ever reads back.
        cfg.spi_3wire = false;
        cfg.use_lock = true;
        cfg.dma_channel = SPI_DMA_CH_AUTO;
        cfg.pin_sclk = BoardConfig::PIN_TFT_SCL;
        cfg.pin_mosi = BoardConfig::PIN_TFT_SDA;
        cfg.pin_miso = -1;
        cfg.pin_dc = BoardConfig::PIN_TFT_DC;
        bus_.config(cfg);
        panel_.setBus(&bus_);
    }

    {
        auto cfg = panel_.config();
        cfg.pin_cs = BoardConfig::PIN_TFT_CS;
        cfg.pin_rst = BoardConfig::PIN_TFT_RES;
        cfg.pin_busy = -1;
        // Native chip resolution before rotation (portrait) -- matches what
        // Adafruit_ST7735::initR(INITR_MINI160x80) set internally
        // (_width=80, _height=160). setRotation(1) at beginLcd() time is
        // what turns this into the 160x80 landscape the board's glass
        // actually shows.
        cfg.panel_width = 80;
        cfg.panel_height = 160;
        cfg.memory_width = 80;
        cfg.memory_height = 160;
        // Same RAM col/row offsets as the old DongleSt7735::setPanelOffset()
        // call (BoardConfig::TFT_COL_START/TFT_ROW_START) -- calibrated
        // against this specific panel, not a LovyanGFX default.
        cfg.offset_x = BoardConfig::TFT_COL_START;
        cfg.offset_y = BoardConfig::TFT_ROW_START;
        cfg.offset_rotation = 0;
        cfg.readable = false;
        cfg.invert = false;
        cfg.rgb_order = false;
        cfg.dlen_16bit = false;
        cfg.bus_shared = false;
        panel_.config(cfg);
    }

    setPanel(&panel_);
}

DonglePeripherals::DonglePeripherals()
    : tft_(),
      ledReady_(false),
      lcdReady_(false),
      lcdBacklightOn_(false),
      lcdBacklightActiveHigh_(false),
      lcdRotation_(1) {
}

void DonglePeripherals::begin() {
    beginLed();
    ledOff();
    beginLcd(lcdRotation_);
}

bool DonglePeripherals::beginLed() {
    gpio_set_direction(BoardConfig::PIN_LED_DI, GPIO_MODE_OUTPUT);
    gpio_set_direction(BoardConfig::PIN_LED_CI, GPIO_MODE_OUTPUT);
    gpio_set_level(BoardConfig::PIN_LED_DI, 0);
    gpio_set_level(BoardConfig::PIN_LED_CI, 0);
    ledReady_ = true;
    return true;
}

void DonglePeripherals::setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness31) {
    if (!ledReady_) {
        beginLed();
    }

    if (brightness31 > 31) {
        brightness31 = 31;
    }

    writeLedFrame(brightness31, r, g, b);
}

void DonglePeripherals::ledOff() {
    setLedColor(0, 0, 0, 1);
}

bool DonglePeripherals::beginLcd(uint8_t rotation) {
    lcdRotation_ = static_cast<uint8_t>(rotation % 4);

    // Keep backlight on while running init sequence.
    setLcdBacklight(true);

    // init()'s default init_impl(use_reset=true, ...) pulses pin_rst itself
    // (config'd in DongleLcd's constructor) -- no separate manual reset
    // sequence needed here, unlike the old Adafruit_ST7735 code this
    // replaces.
    if (!tft_.init()) {
        lcdReady_ = false;
        return false;
    }

    tft_.setRotation(lcdRotation_);
    tft_.fillScreen(0xFFFF);
    tft_.setTextColor(0x0000);
    tft_.setTextSize(1);
    tft_.setTextWrap(true);
    tft_.setCursor(0, 0);

    lcdReady_ = true;
    return true;
}

bool DonglePeripherals::isLcdReady() const {
    return lcdReady_;
}

bool DonglePeripherals::reinitLcd(uint8_t rotation) {
    lcdReady_ = false;
    return beginLcd(rotation);
}

void DonglePeripherals::setLcdRotation(uint8_t rotation) {
    lcdRotation_ = static_cast<uint8_t>(rotation % 4);

    if (!lcdReady_) {
        return;
    }

    tft_.setRotation(lcdRotation_);
    tft_.fillScreen(0x0000);
    tft_.setCursor(0, 0);
}

uint8_t DonglePeripherals::lcdRotation() const {
    return lcdRotation_;
}

DongleLcd* DonglePeripherals::lcd() {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return nullptr;
    }

    return &tft_;
}

void DonglePeripherals::setLcdBacklight(bool on) {
    gpio_set_direction(BoardConfig::PIN_TFT_LED, GPIO_MODE_OUTPUT);
    lcdBacklightOn_ = on;
    const bool pinLevel = lcdBacklightActiveHigh_ ? lcdBacklightOn_ : !lcdBacklightOn_;
    gpio_set_level(BoardConfig::PIN_TFT_LED, pinLevel ? 1 : 0);
}

void DonglePeripherals::setLcdBacklightPolarity(bool activeHigh) {
    lcdBacklightActiveHigh_ = activeHigh;
    setLcdBacklight(lcdBacklightOn_);
}

bool DonglePeripherals::isLcdBacklightOn() const {
    return lcdBacklightOn_;
}

bool DonglePeripherals::isLcdBacklightActiveHigh() const {
    return lcdBacklightActiveHigh_;
}

bool DonglePeripherals::writeLcd(const std::string& text, bool clearFirst, uint16_t color) {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return false;
    }

    setLcdBacklight(true);

    if (clearFirst) {
        tft_.fillScreen(0x0000);
        tft_.setCursor(0, 0);
    }

    tft_.setTextColor(color);
    tft_.println(text.c_str());
    return true;
}

bool DonglePeripherals::clearLcd(uint16_t color) {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return false;
    }

    setLcdBacklight(true);

    tft_.fillScreen(color);
    tft_.setCursor(0, 0);
    return true;
}

void DonglePeripherals::sendLedByte(uint8_t value) const {
    for (int8_t bit = 7; bit >= 0; --bit) {
        gpio_set_level(BoardConfig::PIN_LED_DI, (value & (1U << bit)) ? 1 : 0);
        gpio_set_level(BoardConfig::PIN_LED_CI, 1);
        gpio_set_level(BoardConfig::PIN_LED_CI, 0);
    }
}

void DonglePeripherals::writeLedFrame(uint8_t brightness31, uint8_t r, uint8_t g, uint8_t b) const {
    for (uint8_t i = 0; i < 4; ++i) {
        sendLedByte(0x00);
    }

    sendLedByte(static_cast<uint8_t>(0xE0 | (brightness31 & 0x1F)));
    sendLedByte(b);
    sendLedByte(g);
    sendLedByte(r);

    for (uint8_t i = 0; i < 4; ++i) {
        sendLedByte(0xFF);
    }
}
