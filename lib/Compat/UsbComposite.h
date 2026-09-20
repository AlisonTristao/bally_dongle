#pragma once

// ESP-IDF migration, phase 6 (PLANO_ESPIDF_DONGLE.md secao 7): builds and
// installs the composite USB device (CDC-ACM + HID vendor, one Interface
// Association Descriptor) that used to come for free from arduino-esp32's
// ARDUINO_USB_MODE=0. esp_tinyusb's own Kconfig-driven descriptor
// (usb_descriptors.c inside the component, gated by CFG_TUD_CDC/MSC/etc.)
// never references CFG_TUD_HID at all -- CONFIG_TINYUSB_HID_COUNT only
// turns on the HID class driver itself, nothing wires an HID interface into
// that generated descriptor -- so the whole device/config descriptor lives
// in UsbComposite.cpp instead of reusing esp_tinyusb's default one.
// UsbHidMux.cpp owns the HID report descriptor bytes and the tud_hid_*
// callbacks against the interface this installs; ConsoleCdc.cpp owns
// everything that happens on the CDC interface after tinyusb_cdcacm_init().
namespace UsbComposite {

/** Installs the TinyUSB stack with the composite CDC+HID descriptor. Call
 * once from app_main(), before ConsoleCdc::begin() or any UsbHidMux use --
 * both attach to interfaces this must have already told the stack about. */
bool install();

}  // namespace UsbComposite
