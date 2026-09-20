#include "UsbComposite.h"

#include <cstddef>
#include <cstdint>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

namespace {

const char* kTag = "UsbComposite";

// Interface/endpoint/string numbering for the whole composite device.
// UsbHidMux.cpp's tud_hid_* callbacks rely on there being exactly one HID
// interface (so `instance` is always 0 there) -- that fact is encoded here,
// not passed around, since only this file assembles the descriptor that
// makes it true.
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL,
};

enum {
    EPNUM_CDC_NOTIF = 1,
    EPNUM_CDC = 2,
    EPNUM_HID = 3,
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID,
};

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // IAD required whenever a composite device mixes CDC with another
    // class (tinyusb.h's tinyusb_driver_install() doc comment says the
    // same): bDeviceClass/SubClass/Protocol must be MISC/COMMON/IAD, not
    // CDC's own class code, or Windows fails to group the two CDC
    // interfaces under one COM port.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,   // Espressif VID (TINYUSB_ESPRESSIF_VID in tinyusb.h)
    .idProduct = 0x8A17,  // arbitrary, distinct per composite shape so Windows does not reuse a cached driver from the phase 0-5 boot-only build
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

// Index 0 of a USB string descriptor array is not ASCII -- it is the raw
// two-byte LANGID list (0x0409 = English/US), same convention
// usb_descriptors.c inside the esp_tinyusb component itself uses.
const char kLangId[] = {0x09, 0x04};

const char* kStringDescriptors[] = {
    kLangId,
    "bally_dongle",
    "bally_dongle console+HID",
    "bally-dongle-0001",
    "bally_dongle shell/BTP",
    "bally_dongle HID vendor",
};

// One HID report: kPayloadSize data bytes, no Report ID (TinyUSB's
// TUD_HID_REPORT_DESC_GENERIC_INOUT with no varargs omits the Report ID
// item entirely). UsbHidMux.cpp's tud_hid_report()/tud_hid_set_report_cb
// calls must agree on this exact size.
constexpr std::size_t kHidReportSize = 64;

const uint8_t kHidReportDescriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(kHidReportSize),
};

constexpr std::size_t kConfigTotalLen = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN;

const uint8_t kConfigDescriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, kConfigTotalLen, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP notif address & size, EP data address (out, in) & size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, 0x80 | EPNUM_CDC_NOTIF, 8, EPNUM_CDC, 0x80 | EPNUM_CDC, 64),

    // Interface number, string index, protocol, report descriptor len, EP out & in address, size, polling interval (ms)
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE, sizeof(kHidReportDescriptor), EPNUM_HID,
                             0x80 | EPNUM_HID, kHidReportSize, 5),
};

}  // namespace

// tud_hid_descriptor_report_cb is TinyUSB's own weak callback (C linkage) --
// there is only one HID interface in this device, so `instance` is always
// 0 and the report descriptor is always this one.
extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return kHidReportDescriptor;
}

namespace UsbComposite {

bool install() {
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &kDeviceDescriptor;
    cfg.descriptor.string = kStringDescriptors;
    cfg.descriptor.string_count = sizeof(kStringDescriptors) / sizeof(kStringDescriptors[0]);
    cfg.descriptor.full_speed_config = kConfigDescriptor;
    // ESP32-S3's USB-OTG peripheral is full-speed only (no
    // qualifier/high_speed_config needed -- TUD_OPT_HIGH_SPEED is only set
    // for P4-class targets).

    if (tinyusb_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(kTag, "tinyusb_driver_install() failed");
        return false;
    }
    return true;
}

}  // namespace UsbComposite
