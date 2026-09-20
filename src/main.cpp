// ESP-IDF migration, phase 2+3+4 (see PLANO_ESPIDF_DONGLE.md): "nucleo sem
// periferico" -- ShellOutput (String->std::string) and SerialMux
// (Stream*->ByteIO*) now build under espidf and are wired here just enough
// to prove the whole graph they pull in (BtpTransport, DonglePublisher,
// HubRegistry, HubRelay, ManifestCache, SubscriptionRegistry -- all already
// Arduino-free, "Grupo A" in the plan) links against the real IDF FreeRTOS.
// EspNowManager (phase 3: native Wi-Fi STA bring-up, see its own begin())
// wired in raw, standalone -- EspNowConfig turned out NOT to be the
// mechanical ByteIO swap the plan assumed (see the handoff note this phase
// ends on): its .cpp fully #includes LcdDashboard.h (dereferenced) and
// ShellConfig.h (itself blocked the same way ShellConfig always was), so it
// cannot build yet either. EspNowManager alone has no such dependency (only
// esp_now.h/esp_wifi.h/RadioTxScheduler.h, already clean) and is the actual
// "does the radio come up" proof for this phase.
// ShellConfig/ShellCommandSupport/DongleCommands/DatabaseCommands are
// deliberately NOT part of phase 2/3 (user decision, 2026-09-20): they reach
// into EspNowManager/DonglePeripherals/LcdDashboard/DatabaseStore, and none
// of that is mechanical String/Stream work.
//
// Still boot-only otherwise -- does NOT include AppRuntime.h. AppRuntime and
// most of lib/ still depend on Arduino.h/WiFi.h/USB.h/SD_MMC/Preferences
// (see the plan's Grupo B/C inventory), none of which exist under
// `framework = espidf`. Wiring AppRuntime back in happens incrementally
// across phases 3-7.
//
// LED bit-bang (writeLedFrame/sendLedByte below) is this file's OWN copy,
// kept separate from DonglePeripherals's identical GPIO sequence -- this
// smoke test's status blink (espNowOk green/red, phase 4/5 outcome pulses)
// runs before/after DonglePeripherals ever touches the LED pin and long
// predates it in this file; not worth collapsing the two into a shared
// dependency for a boot-only main.cpp that AppRuntime eventually replaces
// wholesale (secao 5 of the plan).
//
// Phase 6 (PLANO_ESPIDF_DONGLE.md secao 7): UsbComposite::install() brings
// up the hand-built composite CDC+HID descriptor first thing in app_main(),
// before ConsoleCdc/SerialMux/UsbHidMux touch anything USB-side -- console
// is a real transport from here on, not the phase 1-5 no-op stub. The tail
// of app_main() also stops blocking on delay(500) for the LED: SerialMux
// and UsbHidMux both need a tight, non-blocking tick to be usable from a
// real host.
//
// Phase 7 (secao 6, D1): DonglePeripherals/LcdDashboard/StartupConfig now
// build against LovyanGFX instead of Adafruit_GFX/Adafruit_ST7735 -- wired
// in right after the phase 2 nucleus below (LCD/LED bring-up has no
// dependency on ESP-NOW/NVS/SD, so it does not need to wait for phases
// 3-5) as this phase's own smoke test: boot banner on the LCD, then the
// Activity page ticking alongside everything else already running.

#include <stdint.h>

#include "ConsoleCdc.h"
#include "DatabaseStore.h"
#include "DongleKeyStore.h"
#include "DonglePeripherals.h"
#include "DongleSdCard.h"
#include "EspNowManager.h"
#include "LcdDashboard.h"
#include "SerialMux.h"
#include "ShellOutput.h"
#include "StartupConfig.h"
#include "UsbComposite.h"
#include "UsbHidMux.h"
#include "compat.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string>

namespace {

const char* kTag = "boot";

void sendLedByte(uint8_t value) {
    for (int8_t bit = 7; bit >= 0; --bit) {
        gpio_set_level(BoardConfig::PIN_LED_DI, (value & (1U << bit)) ? 1 : 0);
        gpio_set_level(BoardConfig::PIN_LED_CI, 1);
        gpio_set_level(BoardConfig::PIN_LED_CI, 0);
    }
}

void writeLedFrame(uint8_t brightness31, uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < 4; ++i) sendLedByte(0x00);
    sendLedByte(static_cast<uint8_t>(0xE0 | (brightness31 & 0x1F)));
    sendLedByte(b);
    sendLedByte(g);
    sendLedByte(r);
    for (uint8_t i = 0; i < 4; ++i) sendLedByte(0xFF);
}

void blinkPulse(uint8_t r, uint8_t g, uint8_t b, int times) {
    for (int i = 0; i < times; ++i) {
        writeLedFrame(10, r, g, b);
        delay(150);
        writeLedFrame(1, 0, 0, 0);
        delay(150);
    }
}

EspNowManager g_espNow;
LcdDashboard g_dashboard;

// Phase 3 smoke test for "the radio comes up": logs whatever raw ESP-NOW
// datagram arrives, no BTP decode (that is EspNowConfig/ProtocolRouter's
// job, not ported yet -- see this file's header comment). Proves
// EspNowManager::begin()'s native Wi-Fi/ESP-NOW bring-up actually receives
// over the air, which a clean build alone cannot.
void onRawEspNowReceive(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi) {
    (void)data;
    ESP_LOGI("espnow", "rx %u bytes from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d",
             static_cast<unsigned>(len), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             static_cast<int>(rssi));
}

// Placeholder for SerialMux::RunShellLineFn -- the shell itself
// (ShellConfig/TinyShell) is not wired in until a later phase, so any
// COMMAND_REQUEST/TERMINAL_IN reaching this stub just says so instead of
// silently doing nothing.
void stubRunShellLine(const char* commandLine, const char* source, const char* userId,
                      std::string* outFullText) {
    (void)commandLine;
    (void)source;
    (void)userId;
    if (outFullText != nullptr) {
        *outFullText = "shell nao disponivel ainda nesta fase da migracao";
    }
}

}  // namespace

extern "C" void app_main() {
    BoardConfig::initBoardPins(/*lcdBacklightOn=*/false);

    // Phase 6: composite USB (esp_tinyusb CDC+HID) has to exist before
    // anything touches ConsoleCdc -- tinyusb_cdcacm_init() (called from
    // console.begin()) attaches to an interface the low-level descriptor/
    // stack must already know about, and UsbHidMux's tud_hid_* callbacks
    // only start firing once the composite device is installed.
    static ConsoleCdc console;
    const bool usbOk = UsbComposite::install() && console.begin();
    ESP_LOGI("usb", "UsbComposite::install()+ConsoleCdc::begin() -> %s", usbOk ? "OK" : "FAILED");
    UsbHidMux::begin();

    static const char kBanner[] = "bally_dongle phase 6 composite USB up\n";
    console.write(reinterpret_cast<const uint8_t*>(kBanner), sizeof(kBanner) - 1);

    // Phase 2 smoke test: pulls ShellOutput + SerialMux (and everything they
    // transitively depend on) into the real firmware build/link, not just
    // env:native. selfUuid/sourceId/bootId are placeholders -- AppRuntime
    // supplies the real ones once it is reassembled (phase 3+).
    static const std::uint8_t kSelfUuid[16] = {0};
    SerialMux::begin(console, &stubRunShellLine, kSelfUuid, "$ ", /*sourceId=*/0U, /*bootId=*/0U);
    ShellOutput::printTagged(console, "boot", "fase 2: nucleo sem periferico ligado");

    // Phase 7: LCD/LED via LovyanGFX (DonglePeripherals/LcdDashboard,
    // decisao D1). announceBoot() shows the "bally dongle / iniciando..."
    // splash and pulses the LED green -- the same boot banner the old
    // Adafruit-based code drew, now over the real ConsoleCdc for its
    // ShellOutput line instead of Arduino's Serial. dashboard.begin() then
    // takes the panel over for the paged status view; g_dashboard.tick()
    // in the loop below is what actually proves the SPI bus/panel/button
    // chain keeps working past the first frame, not just at boot.
    static DonglePeripherals peripherals;
    StartupConfig::announceBoot(peripherals, console);
    const bool lcdOk = g_dashboard.begin(peripherals);
    ESP_LOGI("lcd", "LcdDashboard::begin() -> %s", lcdOk ? "OK" : "FAILED");

    // Phase 3: native Wi-Fi STA + ESP-NOW bring-up (channel 11, matches
    // AppRuntime's kEspNowChannel). No peers registered yet -- this only
    // proves the radio itself comes up and can receive; pairing/sending
    // waits for EspNowConfig, still blocked (see header comment).
    g_espNow.setReceiveCallback(&onRawEspNowReceive);
    const bool espNowOk = g_espNow.begin(/*channel=*/11U, /*encrypt=*/false);
    ESP_LOGI("espnow", "begin() -> %s", espNowOk ? "OK" : "FAILED");

    // Phase 4 smoke test for "key L survives reboot": nvs_flash_init()
    // already ran as a side effect of g_espNow.begin() above (see
    // EspNowManager::begin()'s comment), so NVS is ready to use here.
    // kTestPassword is a placeholder -- real provisioning goes through
    // "hub -set_key_l" (DongleCommands), still blocked until phase 7 wires
    // ShellConfig back in (see this file's header comment). First boot after
    // flashing: no key in NVS yet, derives and saves one. Every boot after
    // that (power cycle, reset button, no reflash): loadFromNvs() succeeds,
    // which is the actual "survives reboot" proof -- a clean build+flash
    // cannot show that by itself.
    {
        static constexpr char kTestPassword[] = "fase4-teste-nao-e-a-senha-real";
        std::uint8_t verifyTag[DongleKeyStore::kVerifyLength] = {0};

        if (DongleKeyStore::loadFromNvs()) {
            DongleKeyStore::verifyTagL(DongleKeyStore::keyL(), verifyTag);
            ESP_LOGI("keystore", "key L loaded from NVS (survived reboot), verify=%02x%02x%02x%02x",
                     verifyTag[0], verifyTag[1], verifyTag[2], verifyTag[3]);
            blinkPulse(0, 0, 60, 3);  // blue x3: persistence confirmed
        } else {
            std::uint8_t derived[DongleKeyStore::kKeyLength] = {0};
            DongleKeyStore::deriveKeyL(kTestPassword, sizeof(kTestPassword) - 1, derived);
            DongleKeyStore::setKeyL(derived);
            const bool saved = DongleKeyStore::saveToNvs();
            DongleKeyStore::verifyTagL(derived, verifyTag);
            ESP_LOGI("keystore", "key L derived+saveToNvs()=%s, verify=%02x%02x%02x%02x",
                     saved ? "OK" : "FAILED", verifyTag[0], verifyTag[1], verifyTag[2], verifyTag[3]);
            blinkPulse(60, saved ? 60 : 0, 0, 3);  // yellow x3 saved ok, red x3 save failed
        }
    }

    // Phase 5 smoke test: SD mount (DongleSdCard, split out of
    // DonglePeripherals -- see that header's comment) + DatabaseStore
    // (String/File->std::string/POSIX, sqlite3 via the vendored
    // components/esp32-idf-sqlite3). DatabaseStore's paths are all under
    // DongleSdCard::kMountPoint, so it only makes sense to try once the card
    // is actually mounted.
    {
        const bool sdOk = DongleSdCard::begin(/*oneBitMode=*/false);
        if (sdOk) {
            ESP_LOGI("sdcard", "mounted: type=%s oneBit=%d freq=%uKHz total=%lluMB used=%lluMB",
                     DongleSdCard::cardTypeName().c_str(), DongleSdCard::oneBitMode() ? 1 : 0,
                     static_cast<unsigned>(DongleSdCard::frequencyKHz()),
                     static_cast<unsigned long long>(DongleSdCard::totalMB()),
                     static_cast<unsigned long long>(DongleSdCard::usedMB()));
            blinkPulse(60, 60, 60, 3);  // white x3: SD mounted

            static DatabaseStore database;
            if (database.begin(&console)) {
                database.logBootEvent("phase5-smoke-test");
                database.loadPeers(g_espNow);
                std::string status;
                database.getStatus(status);
                ESP_LOGI("database", "begin() OK, status:\n%s", status.c_str());
                blinkPulse(0, 60, 60, 3);  // cyan x3: DB opened
            } else {
                ESP_LOGW("database", "begin() FAILED");
                blinkPulse(60, 30, 0, 3);  // orange x3: DB open failed
            }
        } else {
            ESP_LOGW("sdcard", "mount failed");
            blinkPulse(60, 0, 60, 3);  // magenta x3: SD mount failed
        }
    }

    ESP_LOGI(kTag, "bally_dongle ESP-IDF phase 2+3+4+5+6+7 boot OK, heap free=%u, millis=%u",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(millis()));

    // LED colour is a secondary, no-tooling way to read espNowOk (console
    // now goes out USB-Serial/JTAG on the same COM port used to flash --
    // decisao D4 revisada -- but the LED still works without opening a
    // monitor): dim green blink = Wi-Fi/ESP-NOW bring-up reported success;
    // dim red blink = begin() returned false.
    //
    // Phase 6 replaces the earlier delay(500)-blocked blink loop: SerialMux
    // (shell + BTP over ConsoleCdc) and UsbHidMux (HID echo) both need a
    // tight, non-blocking tick to be usable from a real host -- a 500ms
    // sleep between polls would make the shell feel broken even though the
    // transport itself is fine. The blink keeps its own 500ms cadence via a
    // deadline instead of a sleep.
    uint32_t nextBlinkMs = millis();
    bool on = false;
    while (true) {
        const uint32_t now = millis();
        SerialMux::tick(now);
        UsbHidMux::tick();
        g_dashboard.tick();

        if (static_cast<int32_t>(now - nextBlinkMs) >= 0) {
            nextBlinkMs = now + 500;
            on = !on;
            if (on) {
                if (espNowOk) {
                    writeLedFrame(8, 0, 40, 0);  // dim green
                } else {
                    writeLedFrame(8, 40, 0, 0);  // dim red
                }
            } else {
                writeLedFrame(1, 0, 0, 0);
            }
        }
        delay(2);
    }
}
