#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "compat.h"

// ESP-IDF migration, phase 6 (PLANO_ESPIDF_DONGLE.md secao 7): real ByteIO
// backing for the shell+BTP transport, over the CDC-ACM interface
// UsbComposite::install() wires into the composite descriptor. Replaces the
// phase 1-5 stub that only logged a warning and dropped every write (see
// this file's git history for that version) -- ShellOutput/SerialMux were
// ported against ByteIO long before there was a real transport to back it,
// so this is the only thing that changes for them here.
class ConsoleCdc : public ByteIO {
public:
    // Attaches the ACM line discipline to TINYUSB_CDC_ACM_0. Call once from
    // app_main(), after UsbComposite::install() has run
    // tinyusb_driver_install() -- tinyusb_cdcacm_init() attaches to an
    // interface the low-level descriptor/stack must already know about.
    bool begin();

    size_t write(const uint8_t* data, size_t len) override;
    int read() override;
    int available() override;
    explicit operator bool() const override;

    // Called from the CDC_EVENT_LINE_STATE_CHANGED callback (ConsoleCdc.cpp,
    // file-local, not part of ByteIO) -- public only so that free function
    // can reach it through the single ConsoleCdc instance main.cpp owns.
    void setDtr(bool dtr);

private:
    void refill();

    uint8_t rxBuf_[256] = {0};
    size_t rxLen_ = 0;
    size_t rxPos_ = 0;
    std::atomic<bool> dtr_{false};
};
