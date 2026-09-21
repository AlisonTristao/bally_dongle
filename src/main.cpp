// ESP-IDF migration (see docs/ for the phase history): AppRuntime.cpp now
// carries the real firmware logic again -- USB composite bring-up here is
// the only thing that has to happen before AppRuntime::begin() touches the
// console (tinyusb_cdcacm_init() attaches to an interface the low-level
// descriptor/stack must already know about).

#include "AppRuntime.h"
#include "ConsoleCdc.h"
#include "UsbComposite.h"
#include "compat.h"
#include "config.h"
#include "esp_log.h"

extern "C" void app_main() {
    BoardConfig::initBoardPins(/*lcdBacklightOn=*/false);

    // Composite USB (esp_tinyusb CDC+HID) has to exist before anything
    // touches ConsoleCdc -- tinyusb_cdcacm_init() (called from
    // console.begin()) attaches to an interface the low-level descriptor/
    // stack must already know about, and UsbHidMux's tud_hid_* callbacks
    // (started from AppRuntime::begin()) only start firing once the
    // composite device is installed.
    static ConsoleCdc console;
    const bool usbOk = UsbComposite::install() && console.begin();
    ESP_LOGI("usb", "UsbComposite::install()+ConsoleCdc::begin() -> %s", usbOk ? "OK" : "FAILED");

    static AppRuntime app;
    app.begin(console);

    while (true) {
        app.tick();
    }
}
