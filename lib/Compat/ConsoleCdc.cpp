#include "ConsoleCdc.h"

#include "esp_log.h"
#include "tinyusb_cdc_acm.h"

#if defined(DONGLE_USB_AUTORESET)
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_persist.h"
#include "esp_system.h"
#endif

namespace {

const char* kTag = "ConsoleCdc";

// tinyusb_cdcacm_register_callback's callbacks are plain C function
// pointers with no user-arg slot, so they need somewhere to reach the one
// ConsoleCdc instance main.cpp owns. There is exactly one CDC interface in
// this firmware (TINYUSB_CDC_ACM_0), so a single file-local pointer is
// enough -- no registry needed.
ConsoleCdc* g_instance = nullptr;

void onLineStateChanged(int itf, cdcacm_event_t* event) {
    (void)itf;
    if (g_instance == nullptr || event == nullptr) return;
    g_instance->setDtr(event->line_state_changed_data.dtr);
}

#if defined(DONGLE_USB_AUTORESET)
void onLineCodingChanged(int itf, cdcacm_event_t* event) {
    (void)itf;
    if (event == nullptr || event->line_coding_changed_data.p_line_coding == nullptr) return;

    // esptool.py's classic "soft reset into bootloader" sequence opens the
    // port at 1200 baud and closes it again -- the same trick arduino-esp32's
    // USBCDC does internally on every board that ships without a hardware
    // auto-reset circuit. Gated behind DONGLE_USB_AUTORESET (plan sec. 7,
    // item 3; opposite polarity of the old DONGLE_USB_NO_AUTORESET opt-out)
    // so a field build never jumps to the ROM bootloader just because some
    // host driver probed the line coding during enumeration.
    if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
        ESP_LOGW(kTag, "1200-baud touch detected, rebooting to bootloader");
        chip_usb_set_persist_flags(USBDC_BOOT_DFU);
        esp_restart();
    }
}
#endif

}  // namespace

bool ConsoleCdc::begin() {
    g_instance = this;

    tinyusb_config_cdcacm_t cfg = {};
    cfg.cdc_port = TINYUSB_CDC_ACM_0;
    cfg.callback_line_state_changed = &onLineStateChanged;
#if defined(DONGLE_USB_AUTORESET)
    cfg.callback_line_coding_changed = &onLineCodingChanged;
#endif

    if (tinyusb_cdcacm_init(&cfg) != ESP_OK) {
        ESP_LOGE(kTag, "tinyusb_cdcacm_init() failed");
        return false;
    }
    return true;
}

void ConsoleCdc::setDtr(bool dtr) { dtr_.store(dtr); }

size_t ConsoleCdc::write(const uint8_t* data, size_t len) {
    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    // Non-blocking flush (timeout_ticks=0): ShellOutput/SerialMux call
    // write() a lot per line, so blocking here would serialize the whole
    // shell behind USB IN completion. TinyUSB keeps queuing/flushing on its
    // own task in the background either way.
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return queued;
}

void ConsoleCdc::refill() {
    if (rxPos_ < rxLen_) return;
    size_t got = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rxBuf_, sizeof(rxBuf_), &got) != ESP_OK) {
        got = 0;
    }
    rxLen_ = got;
    rxPos_ = 0;
}

int ConsoleCdc::available() {
    refill();
    return static_cast<int>(rxLen_ - rxPos_);
}

int ConsoleCdc::read() {
    refill();
    if (rxPos_ >= rxLen_) return -1;
    return rxBuf_[rxPos_++];
}

ConsoleCdc::operator bool() const { return dtr_.load(); }
