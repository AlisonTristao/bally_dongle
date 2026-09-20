#pragma once

/**
 * @brief Hardware bring-up spike for the dongle's second USB interface (BTP
 * v1.1.0 "usb_hid" transport profile,
 * BTP/docs/fragmentation-and-transports.md section 3.3).
 *
 * ESP-IDF migration, phase 6 (PLANO_ESPIDF_DONGLE.md secao 7): ported from
 * arduino-esp32's USBHIDVendor to raw TinyUSB (tud_hid_* weak callbacks,
 * implemented in UsbHidMux.cpp) against the HID interface
 * UsbComposite::install() wires into the composite descriptor. Same
 * echo-only behaviour as before this port -- begin()/tick() only bounce
 * whatever bytes the host writes back out, no BTP framing wired in yet --
 * validating on real hardware that:
 *
 * - the host still enumerates both the CDC port and the HID vendor
 *   interface simultaneously under the hand-built descriptor
 *   (UsbComposite.cpp), now that neither comes from arduino-esp32's
 *   ARDUINO_USB_MODE=0 nor from esp_tinyusb's own Kconfig-driven descriptor
 *   generator (which does not wire up HID at all -- see UsbComposite.cpp's
 *   header comment);
 * - the shell/BTP console (SerialMux, now over ConsoleCdc/tinyusb_cdcacm)
 *   survives alongside sustained HID traffic;
 * - the HID class driver TinyUSB itself provides needs no more than the
 *   handful of tud_hid_* callbacks this file implements.
 *
 * Once validated, this module grows into the real transport: BTP session
 * wiring (reusing or trimming SerialSession, see its own header), a new
 * SudoManager/SubscriptionRegistry client identity prefix, and TX/RX
 * priority queues matching SerialMux's shape. No BTP dependency yet on
 * purpose, so this spike can be flashed and tested in isolation.
 */
namespace UsbHidMux {

/** Marks the HID vendor interface ready to echo. Call once from
 * app_main()/AppRuntime::begin(), after UsbComposite::install() has already
 * wired the HID interface into the TinyUSB stack -- unlike the old
 * USBHIDVendor, there is no separate begin() on the TinyUSB side to call
 * here, tud_hid_* callbacks are live as soon as the composite device is
 * installed. */
void begin();

/** Sends back whatever the most recent tud_hid_set_report_cb received, once
 * the IN endpoint is free. Call once per AppRuntime::tick(); a fast no-op
 * when nothing is pending. */
void tick();

} // namespace UsbHidMux
