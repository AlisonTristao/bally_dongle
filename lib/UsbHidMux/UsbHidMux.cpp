#include "UsbHidMux.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "class/hid/hid_device.h"

namespace UsbHidMux {
namespace {

// Must match UsbComposite.cpp's kHidReportSize -- that file declares this
// many bytes per HID report in the descriptor, these callbacks fill/drain
// exactly that many. Not shared via a header on purpose: UsbComposite owns
// the wire-level HID interface (descriptor, endpoint sizes), this is the
// application-level payload convention layered on top of it, the same
// separation ConsoleCdc/tinyusb_cdcacm already has.
constexpr std::size_t kReportSize = 64;

// prepend_size convention carried over from the arduino-esp32 USBHIDVendor
// bring-up this replaces: buffer[0] is an explicit valid-length prefix for
// the payload bytes that follow, because a fixed-size HID report always
// transmits all kReportSize octets, zero-padded by the USB stack when a
// write is shorter -- without this the receiver has no way to tell real
// data from padding.
constexpr std::size_t kPayloadSize = kReportSize - 1;

std::uint8_t g_pendingOut[kReportSize] = {0};
std::atomic<bool> g_hasPendingOut{false};
std::atomic<bool> g_started{false};

} // namespace

void begin() {
    g_started.store(true);
}

void tick() {
    if (!g_started.load()) return;
    if (!g_hasPendingOut.load()) return;
    if (!tud_hid_ready()) return;

    // Report ID 0 (none): the descriptor in UsbComposite.cpp declares no
    // Report ID item for this interface.
    if (tud_hid_report(0, g_pendingOut, sizeof(g_pendingOut))) {
        g_hasPendingOut.store(false);
    }
}

} // namespace UsbHidMux

// tud_hid_* are TinyUSB's own weak callbacks (C linkage, declared in
// class/hid/hid_device.h). There is exactly one HID interface in this
// composite device (UsbComposite.cpp's ITF_NUM_HID), so `instance` is
// always 0.

extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t instance, std::uint8_t report_id,
                                               hid_report_type_t report_type, std::uint8_t* buffer,
                                               std::uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    // This interface is host-write/device-echo only (matches the old
    // USBHIDVendor spike) -- no GET_REPORT data to hand back.
    return 0;
}

extern "C" void tud_hid_set_report_cb(std::uint8_t instance, std::uint8_t report_id, hid_report_type_t report_type,
                                      std::uint8_t const* buffer, std::uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    if (buffer == nullptr || bufsize == 0) return;

    // buffer[0] is the host's own valid-length prefix (see kPayloadSize
    // above); clamp defensively instead of trusting an out-of-range value.
    const std::uint8_t validLength =
        buffer[0] > UsbHidMux::kPayloadSize ? UsbHidMux::kPayloadSize : buffer[0];
    if (validLength == 0) return;

    UsbHidMux::g_pendingOut[0] = validLength;
    std::memcpy(UsbHidMux::g_pendingOut + 1, buffer + 1, validLength);
    UsbHidMux::g_hasPendingOut.store(true);
}
