#pragma once

// ESP-IDF migration, phase 1 (see PLANO_ESPIDF_DONGLE.md, secao 4): the one
// place that replaces Arduino's millis()/delay() and the Stream&-based I/O
// interface (ShellOutput/SerialMux/EspNowConfig's `Stream* g_io`). Ported
// libs take a ByteIO& instead of a Stream&; ConsoleCdc (lib/Compat) is the
// concrete implementation, backed by nothing yet in phase 1 and by
// tinyusb_cdcacm once phase 6 brings up the composite USB device.

#include <cstddef>
#include <cstdint>
#include <cstring>

// Grupo A/B libs (ShellOutput, SerialMux, ...) build both for the firmware
// (`framework = espidf`, ESP_PLATFORM defined globally by the IDF build
// system) and for [env:native]'s plain-host toolchain, which has neither
// esp_timer.h nor FreeRTOS. Keep millis()/delay() real on-device and a
// host-clock/no-op-ish equivalent under env:native -- none of today's
// native tests exercise real timing through these (ShellOutput's delay(1) is
// a zero-write retry path a test-double IO never triggers), so the native
// side only has to be a valid, harmless stand-in.
#if defined(ESP_PLATFORM)

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#else

#include <chrono>
#include <thread>

inline uint32_t millis() {
    using namespace std::chrono;
    static const auto kStart = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - kStart).count());
}

inline void delay(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif

struct ByteIO {
    virtual size_t write(const uint8_t* data, size_t len) = 0;
    virtual int read() = 0;          // -1 if empty
    virtual int available() = 0;
    virtual explicit operator bool() const = 0;  // DTR / port open

    // Non-virtual convenience wrappers over the one real virtual write() --
    // ShellOutput's line formatting writes a lot of single characters and
    // C-string literals (mirroring Arduino's Print::write(char)/print(const
    // char*), which every one of its call sites was written against).
    size_t write(uint8_t byte) { return write(&byte, 1); }
    size_t print(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)); }

protected:
    ~ByteIO() = default;
};
