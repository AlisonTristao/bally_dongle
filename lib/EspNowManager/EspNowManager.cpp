#include "EspNowManager.h"

#include "compat.h"

#include <atomic>
#include <cstring>

#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Active instance used by static C callbacks required by ESP-NOW API.
EspNowManager* EspNowManager::activeInstance_ = nullptr;

namespace {

constexpr std::size_t kTxPriorityCount = RadioTxScheduler::kPriorityCount;
constexpr UBaseType_t kTxQueueDepth[kTxPriorityCount] = {
    6U,  // Critical: heartbeat, COMMAND, CONTROL
    4U,  // Control: LOG/TERMINAL and ordinary management
    12U, // Data: TELEMETRY burst absorption
};
constexpr std::uint8_t kNoCompletionSlot = 0xFFU;
constexpr std::size_t kCompletionSlotCount = 4U;
// 60 ms, down from 250: this is the per-frame budget the single TX worker
// waits for an ordinary fire-and-forget send's callback (sendToMac --
// relay, MANIFEST_DATA replies, LOG, COMMAND_RESULT, manifest priming --
// the bulk of this dongle's radio output) before giving up and moving to
// the next queued frame. Same reasoning bally_OS's TxScheduler::configure()
// already applies to its own delivery timeout: "a callback that has not
// arrived in N ms is not going to change the outcome; the frame was
// already handed to the driver". At 250 ms, a single missing/late callback
// (RF noise near the motors) stalled every other frame behind it in this
// worker's queues for a quarter second each; near-continuous loss collapsed
// heartbeat, manifest priming and relay to a crawl even after the
// unbounded-quarantine bug below was fixed.
constexpr std::uint32_t kAsyncCallbackTimeoutMs = 60U;
// ESP-NOW callbacks normally arrive before the caller's timeout.  If one does
// not, wait briefly for a genuinely late callback so it cannot be mistaken for
// the next frame, but never wait forever: an absent robot/driver callback used
// to park the only TX worker until the dongle was rebooted.
//
// 100 ms, down from 500: this grace only needs to outlast a callback that is
// late but still coming, not absorb a whole missing one -- kAsyncCallbackTimeoutMs
// above already spent its own budget waiting for that. At 500 ms a single lost
// callback held the only TX worker for up to 750 ms (250 + 500) before this
// pass; now the worst case per lost callback is 160 ms (60 + 100).
constexpr std::uint32_t kLateCallbackGraceMs = 100U;

struct TxRequest {
    std::uint8_t mac[6];
    std::uint16_t len;
    std::uint8_t data[EspNowManager::MAX_DATA_LEN];
    std::uint8_t completionSlot;
    std::uint32_t completionGeneration;
    std::uint32_t enqueuedMs;
    std::uint32_t timeoutMs;
};

struct DriverStatus {
    std::uint8_t mac[6];
    esp_now_send_status_t status;
};

struct CompletionSlot {
    SemaphoreHandle_t signal = nullptr;
    bool inUse = false;
    std::uint32_t generation = 0U;
    bool callbackReceived = false;
    bool delivered = false;
};

QueueHandle_t g_txQueues[kTxPriorityCount] = {nullptr, nullptr, nullptr};
QueueHandle_t g_driverStatusQueue = nullptr;
TaskHandle_t g_txWorkerTask = nullptr;
volatile bool g_txWorkerRunning = false;
CompletionSlot g_completionSlots[kCompletionSlotCount];
portMUX_TYPE g_completionMux = portMUX_INITIALIZER_UNLOCKED;
// std::atomic, not `volatile` (ESP-IDF migration, PLANO_ESPIDF_DONGLE.md
// phase 3): GCC 15's `-Werror=volatile` rejects `++` on volatile (deprecated
// in C++20). atomic is the correct replacement, not a warning-silencer --
// volatile never made cross-task increments atomic, only stopped the
// compiler from caching the value in a register. g_txWorkerRunning above
// stays `volatile bool`: nothing here does a compound op on it, only plain
// reads/assignments, which are not affected by this deprecation.
std::atomic<std::uint32_t> g_txEnqueued[kTxPriorityCount] = {};
std::atomic<std::uint32_t> g_txDroppedQueueFull[kTxPriorityCount] = {};
std::atomic<std::uint32_t> g_txDriverRejected{0U};
std::atomic<std::uint32_t> g_txCallbackTimeouts{0U};
std::atomic<std::uint32_t> g_txCallbacksReceived{0U};

std::size_t priorityIndex(EspNowManager::TxPriority priority) noexcept {
    const std::size_t index = static_cast<std::size_t>(priority);
    return index < kTxPriorityCount ? index :
        static_cast<std::size_t>(EspNowManager::TxPriority::Control);
}

bool completionStillActive(const TxRequest& request) noexcept {
    if (request.completionSlot == kNoCompletionSlot ||
        request.completionSlot >= kCompletionSlotCount) {
        return true;
    }
    bool active = false;
    portENTER_CRITICAL(&g_completionMux);
    const CompletionSlot& slot = g_completionSlots[request.completionSlot];
    active = slot.inUse && slot.generation == request.completionGeneration;
    portEXIT_CRITICAL(&g_completionMux);
    return active;
}

void completeRequest(const TxRequest& request, bool callbackReceived, bool delivered) noexcept {
    if (request.completionSlot == kNoCompletionSlot ||
        request.completionSlot >= kCompletionSlotCount) {
        return;
    }

    SemaphoreHandle_t signal = nullptr;
    portENTER_CRITICAL(&g_completionMux);
    CompletionSlot& slot = g_completionSlots[request.completionSlot];
    if (slot.inUse && slot.generation == request.completionGeneration) {
        slot.callbackReceived = callbackReceived;
        slot.delivered = delivered;
        signal = slot.signal;
    }
    portEXIT_CRITICAL(&g_completionMux);

    if (signal != nullptr) {
        xSemaphoreGive(signal);
    }
}

bool dequeueScheduled(TxRequest& out, std::size_t& scheduleCursor) noexcept {
    bool available[kTxPriorityCount]{};
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        available[i] = g_txQueues[i] != nullptr && uxQueueMessagesWaiting(g_txQueues[i]) > 0U;
    }

    const RadioTxScheduler::Selection selected =
        RadioTxScheduler::choose(available, scheduleCursor);
    scheduleCursor = selected.nextCursor;
    if (!selected.found) {
        return false;
    }
    return xQueueReceive(g_txQueues[priorityIndex(selected.priority)], &out, 0) == pdTRUE;
}

void txWorker(void*) {
    std::size_t scheduleCursor = 0U;
    while (g_txWorkerRunning) {
        TxRequest request{};
        if (!dequeueScheduled(request, scheduleCursor)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // A synchronous caller may have timed out while this request waited
        // behind earlier frames. Cancel before touching the driver.
        if (!completionStillActive(request)) {
            continue;
        }

        std::uint32_t callbackBudgetMs = kAsyncCallbackTimeoutMs;
        if (request.completionSlot != kNoCompletionSlot) {
            const std::uint32_t elapsed = millis() - request.enqueuedMs;
            if (elapsed >= request.timeoutMs) {
                completeRequest(request, false, false);
                continue;
            }
            callbackBudgetMs = request.timeoutMs - elapsed;
        }

        // There is exactly one in-flight driver send. Clearing stale status
        // before it starts makes a callback unambiguously belong to this MAC.
        xQueueReset(g_driverStatusQueue);
        if (esp_now_send(request.mac, request.data, request.len) != ESP_OK) {
            ++g_txDriverRejected;
            completeRequest(request, false, false);
            continue;
        }

        const std::uint32_t waitStartedMs = millis();
        bool callbackReceived = false;
        bool delivered = false;
        while ((millis() - waitStartedMs) < callbackBudgetMs) {
            const std::uint32_t elapsed = millis() - waitStartedMs;
            const std::uint32_t remainingMs = callbackBudgetMs - elapsed;
            DriverStatus status{};
            const TickType_t waitTicks = pdMS_TO_TICKS(remainingMs > 0U ? remainingMs : 1U);
            if (xQueueReceive(g_driverStatusQueue, &status, waitTicks) != pdTRUE) {
                break;
            }
            if (std::memcmp(status.mac, request.mac, sizeof(request.mac)) != 0) {
                continue; // defensive stale callback from a previous timeout
            }
            callbackReceived = true;
            delivered = status.status == ESP_NOW_SEND_SUCCESS;
            ++g_txCallbacksReceived;
            break;
        }
        if (!callbackReceived) {
            ++g_txCallbackTimeouts;
            completeRequest(request, false, false);

            // ESP-NOW does not carry an application token in its callback.
            // Give a late callback a short chance to arrive before reusing the
            // status queue, then discard anything stale and resume. An
            // unbounded quarantine here made one missing callback permanently
            // stop heartbeat, manifest and relay traffic.
            const std::uint32_t graceStartedMs = millis();
            while (g_txWorkerRunning &&
                   (millis() - graceStartedMs) < kLateCallbackGraceMs) {
                DriverStatus late{};
                const std::uint32_t elapsedGrace = millis() - graceStartedMs;
                const std::uint32_t remainingGrace = kLateCallbackGraceMs - elapsedGrace;
                if (xQueueReceive(g_driverStatusQueue, &late,
                                  pdMS_TO_TICKS(remainingGrace > 0U ? remainingGrace : 1U)) == pdTRUE &&
                    std::memcmp(late.mac, request.mac, sizeof(request.mac)) == 0) {
                    ++g_txCallbacksReceived;
                    break;
                }
            }
            // The late callback queue has no correlation token. Clearing it is
            // safer than letting an old result satisfy a future request after
            // the bounded recovery window.
            xQueueReset(g_driverStatusQueue);
            continue;
        }
        completeRequest(request, callbackReceived, delivered);
    }
    vTaskDelete(nullptr);
}

void destroyTxSchedulerStorage() noexcept {
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        if (g_txQueues[i] != nullptr) {
            vQueueDelete(g_txQueues[i]);
            g_txQueues[i] = nullptr;
        }
    }
    if (g_driverStatusQueue != nullptr) {
        vQueueDelete(g_driverStatusQueue);
        g_driverStatusQueue = nullptr;
    }
    for (CompletionSlot& slot : g_completionSlots) {
        if (slot.signal != nullptr) {
            vSemaphoreDelete(slot.signal);
        }
        slot = {};
    }
}

bool startTxScheduler() noexcept {
    destroyTxSchedulerStorage();
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        g_txQueues[i] = xQueueCreate(kTxQueueDepth[i], sizeof(TxRequest));
        if (g_txQueues[i] == nullptr) {
            destroyTxSchedulerStorage();
            return false;
        }
        g_txEnqueued[i] = 0U;
        g_txDroppedQueueFull[i] = 0U;
    }
    g_driverStatusQueue = xQueueCreate(4U, sizeof(DriverStatus));
    if (g_driverStatusQueue == nullptr) {
        destroyTxSchedulerStorage();
        return false;
    }
    for (CompletionSlot& slot : g_completionSlots) {
        slot.signal = xSemaphoreCreateBinary();
        if (slot.signal == nullptr) {
            destroyTxSchedulerStorage();
            return false;
        }
    }

    g_txDriverRejected = 0U;
    g_txCallbackTimeouts = 0U;
    g_txCallbacksReceived = 0U;
    g_txWorkerRunning = true;
    if (xTaskCreate(txWorker, "espnow_tx", 4096U, nullptr, 3U, &g_txWorkerTask) != pdPASS) {
        g_txWorkerRunning = false;
        g_txWorkerTask = nullptr;
        destroyTxSchedulerStorage();
        return false;
    }
    return true;
}

void stopTxScheduler() noexcept {
    g_txWorkerRunning = false;
    if (g_txWorkerTask != nullptr) {
        vTaskDelete(g_txWorkerTask);
        g_txWorkerTask = nullptr;
    }
}

bool enqueueRequest(const TxRequest& request, EspNowManager::TxPriority priority) noexcept {
    const std::size_t index = priorityIndex(priority);
    if (!g_txWorkerRunning || g_txWorkerTask == nullptr || g_txQueues[index] == nullptr ||
        xQueueSend(g_txQueues[index], &request, 0) != pdTRUE) {
        ++g_txDroppedQueueFull[index];
        return false;
    }
    ++g_txEnqueued[index];
    xTaskNotifyGive(g_txWorkerTask);
    return true;
}

int acquireCompletionSlot(std::uint32_t& outGeneration) noexcept {
    int selected = -1;
    portENTER_CRITICAL(&g_completionMux);
    for (std::size_t i = 0U; i < kCompletionSlotCount; ++i) {
        CompletionSlot& slot = g_completionSlots[i];
        if (!slot.inUse && slot.signal != nullptr) {
            slot.inUse = true;
            ++slot.generation;
            if (slot.generation == 0U) ++slot.generation;
            slot.callbackReceived = false;
            slot.delivered = false;
            outGeneration = slot.generation;
            selected = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&g_completionMux);

    if (selected >= 0) {
        while (xSemaphoreTake(g_completionSlots[selected].signal, 0) == pdTRUE) {}
    }
    return selected;
}

void releaseCompletionSlot(std::size_t index, std::uint32_t generation) noexcept {
    if (index >= kCompletionSlotCount) return;
    portENTER_CRITICAL(&g_completionMux);
    CompletionSlot& slot = g_completionSlots[index];
    if (slot.inUse && slot.generation == generation) {
        slot.inUse = false;
        slot.callbackReceived = false;
        slot.delivered = false;
    }
    portEXIT_CRITICAL(&g_completionMux);
}

} // namespace

// Build empty manager state; runtime init happens in begin().
EspNowManager::EspNowManager()
    : deviceCount_(0),
      initialized_(false),
      channel_(0),
      encrypt_(false),
      receiveCallback_(nullptr),
      sendCallback_(nullptr) {
}

// Initialize Wi-Fi station mode and register ESP-NOW callbacks.
bool EspNowManager::begin(uint8_t channel, bool encrypt) {
    channel_ = channel;
    encrypt_ = encrypt;

    // Native Wi-Fi STA bring-up (ESP-IDF migration, PLANO_ESPIDF_DONGLE.md
    // phase 3), replacing what arduino-esp32's WiFi.mode(WIFI_STA)/
    // WiFi.disconnect() used to do under the hood. Mirrors bally_OS's own
    // ROBOT::configureCommunication() (BallyRobot.cpp) -- the Wi-Fi driver
    // needs NVS for calibration data before esp_wifi_init() will succeed.
    // DongleKeyStore::loadFromNvs() already inits it earlier in boot, so this
    // call is normally a no-op -- kept so begin() does not depend on that
    // ordering.
    esp_err_t nvsResult = nvs_flash_init();
    if (nvsResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvsResult = nvs_flash_init();
    }
    if (nvsResult != ESP_OK) {
        initialized_ = false;
        return false;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    // Not strictly needed for ESP-NOW alone (no DHCP client on this side of
    // the link), but esp_wifi_start() expects the default STA netif to
    // exist -- same ordering bally_OS's configureCommunication() documents.
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        initialized_ = false;
        return false;
    }

    wifi_init_config_t wifiInitConfig = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifiInitConfig) != ESP_OK) {
        initialized_ = false;
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        initialized_ = false;
        return false;
    }

    // Default modem sleep lets the radio doze between beacons, adding
    // latency to both TX and RX -- same reasoning and same fix as
    // bally_OS's own esp_wifi_set_ps(WIFI_PS_NONE) (BallyRobot.cpp,
    // configureCommunication()). This side of the link never had it applied
    // at all until now.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // peerInfo.channel below (0 = "current channel") only WORKS if the radio
    // is actually already on the channel a peer expects -- esp_now_add_peer()
    // rejects a non-zero peerInfo.channel that disagrees with this, so a
    // requested channel has to be applied here, before any peer exists.
    if (channel != 0) {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }

    if (esp_now_init() != ESP_OK) {
        initialized_ = false;
        return false;
    }

    // A fixed, explicit rate instead of the driver's default (legacy
    // 802.11b, ~1 Mbps): shrinks per-frame airtime on every peer this
    // interface talks to. ESP-IDF migration phase 3: the arduino-esp32-only
    // `esp_wifi_config_espnow_rate()` interface-wide call this used to be
    // does not exist under real ESP-IDF (6.0.1 here) -- esp_now.h only ever
    // had the PER-PEER `esp_now_set_peer_rate_config()`, same as bally_OS
    // already uses (BallyRobot.cpp, configureCommunication()). Applied in
    // addPeerToEspNow() below, once per peer, after esp_now_add_peer().
    // MCS5_SGI matches the rate configured on the robot side; walk both up
    // together if bench margin allows it.

    initialized_ = true;
    activeInstance_ = this;

    // Bind static handlers, then restore already registered peers.
    esp_now_register_recv_cb(handleReceiveStatic);
    esp_now_register_send_cb(handleSendStatic);

    if (!startTxScheduler()) {
        esp_now_deinit();
        initialized_ = false;
        activeInstance_ = nullptr;
        return false;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        if (!addPeerToEspNow(devices_[i].mac)) {
            stopTxScheduler();
            esp_now_deinit();
            destroyTxSchedulerStorage();
            initialized_ = false;
            activeInstance_ = nullptr;
            return false;
        }
    }

    return true;
}

// Deinitialize ESP-NOW and detach this instance from static callback dispatch.
void EspNowManager::end() {
    stopTxScheduler();
    if (initialized_) {
        esp_now_deinit();
    }
    destroyTxSchedulerStorage();

    initialized_ = false;
    if (activeInstance_ == this) {
        activeInstance_ = nullptr;
    }
}

// Add one device to local registry and to ESP-NOW runtime when active.
bool EspNowManager::addDevice(const uint8_t mac[6], const char* name, const char* description) {
    if (mac == nullptr || deviceCount_ >= MAX_DEVICES) {
        return false;
    }

    if (findDeviceIndexByMac(mac) >= 0) {
        return false;
    }

    if (initialized_ && !addPeerToEspNow(mac)) {
        return false;
    }

    deviceInfo item = {};
    memcpy(item.mac, mac, sizeof(item.mac));
    copyText(item.name, sizeof(item.name), name);
    copyText(item.description, sizeof(item.description), description);

    devices_[deviceCount_] = item;
    ++deviceCount_;
    return true;
}

// Convenience overload to add from a prefilled struct.
bool EspNowManager::addDevice(const deviceInfo& device) {
    return addDevice(device.mac, device.name, device.description);
}

// Remove one device by index and compact local registry array.
bool EspNowManager::removeDeviceByIndex(size_t index) {
    if (index >= deviceCount_) {
        return false;
    }

    if (initialized_) {
        removePeerFromEspNow(devices_[index].mac);
    }

    for (size_t i = index; i + 1 < deviceCount_; ++i) {
        devices_[i] = devices_[i + 1];
    }

    --deviceCount_;
    devices_[deviceCount_] = {};
    return true;
}

// Remove one device by MAC when present.
bool EspNowManager::removeDeviceByMac(const uint8_t mac[6]) {
    const int index = findDeviceIndexByMac(mac);
    if (index < 0) {
        return false;
    }

    return removeDeviceByIndex(static_cast<size_t>(index));
}

// Update one device metadata by index.
bool EspNowManager::updateDeviceByIndex(size_t index, const char* name, const char* description) {
    if (index >= deviceCount_) {
        return false;
    }

    copyText(devices_[index].name, sizeof(devices_[index].name), name);
    copyText(devices_[index].description, sizeof(devices_[index].description), description);
    return true;
}

// Update one device metadata by MAC.
bool EspNowManager::updateDeviceByMac(const uint8_t mac[6], const char* name, const char* description) {
    const int index = findDeviceIndexByMac(mac);
    if (index < 0) {
        return false;
    }

    return updateDeviceByIndex(static_cast<size_t>(index), name, description);
}

// Remove all devices from both local storage and ESP-NOW peer table.
void EspNowManager::clearDevices() {
    if (initialized_) {
        for (size_t i = 0; i < deviceCount_; ++i) {
            removePeerFromEspNow(devices_[i].mac);
        }
    }

    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        devices_[i] = {};
    }
    deviceCount_ = 0;
}

// Number of currently registered devices.
size_t EspNowManager::deviceCount() const {
    return deviceCount_;
}

// Read one device entry by index.
bool EspNowManager::deviceAt(size_t index, deviceInfo& outDevice) const {
    if (index >= deviceCount_) {
        return false;
    }

    outDevice = devices_[index];
    return true;
}

// Return pointer to internal list for read-only iteration.
const EspNowManager::deviceInfo* EspNowManager::deviceList() const {
    return devices_;
}

// Copy local registry into caller buffer with maxItems bound.
size_t EspNowManager::copyDeviceList(deviceInfo* outList, size_t maxItems) const {
    if (outList == nullptr || maxItems == 0) {
        return 0;
    }

    const size_t total = (deviceCount_ < maxItems) ? deviceCount_ : maxItems;
    for (size_t i = 0; i < total; ++i) {
        outList[i] = devices_[i];
    }

    return total;
}

// Public lookup helper for MAC address.
int EspNowManager::deviceIndexByMac(const uint8_t mac[6]) const {
    return findDeviceIndexByMac(mac);
}

// Send one datagram to every registered device.
bool EspNowManager::sendToAll(const uint8_t* data, size_t len) const {
    if (!initialized_ || deviceCount_ == 0) {
        return false;
    }

    bool sentAtLeastOne = false;
    for (size_t i = 0; i < deviceCount_; ++i) {
        if (sendToDevice(i, data, len)) {
            sentAtLeastOne = true;
        }
    }

    return sentAtLeastOne;
}

// Send one datagram by device index.
bool EspNowManager::sendToDevice(size_t index, const uint8_t* data, size_t len) const {
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMac(devices_[index].mac, data, len);
}

// Enqueue one copied datagram. Only txWorker() calls esp_now_send(), so driver
// callbacks can never be consumed by another producer's wait slot.
bool EspNowManager::sendToMac(const uint8_t mac[6], const uint8_t* data, size_t len,
                              TxPriority priority) const {
    if (!initialized_ || mac == nullptr || data == nullptr || len == 0 || len > MAX_DATA_LEN) {
        return false;
    }

    TxRequest request{};
    std::memcpy(request.mac, mac, sizeof(request.mac));
    request.len = static_cast<std::uint16_t>(len);
    std::memcpy(request.data, data, len);
    request.completionSlot = kNoCompletionSlot;
    request.enqueuedMs = millis();
    request.timeoutMs = kAsyncCallbackTimeoutMs;
    return enqueueRequest(request, priority);
}

// Send to one index and wait for callback delivery status.
bool EspNowManager::sendToDeviceWithStatus(size_t index, const uint8_t* data, size_t len, bool& outDelivered, uint32_t timeoutMs) const {
    outDelivered = false;
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMacWithStatus(devices_[index].mac, data, len, outDelivered, timeoutMs);
}

// Send to one MAC and wait for callback delivery status.
bool EspNowManager::sendToMacWithStatus(const uint8_t mac[6], const uint8_t* data, size_t len,
                                        bool& outDelivered, uint32_t timeoutMs,
                                        TxPriority priority) const {
    outDelivered = false;
    if (!initialized_ || mac == nullptr || data == nullptr || len == 0 ||
        len > MAX_DATA_LEN || timeoutMs == 0U) {
        return false;
    }

    std::uint32_t generation = 0U;
    const int slotIndex = acquireCompletionSlot(generation);
    if (slotIndex < 0) {
        return false;
    }

    TxRequest request{};
    std::memcpy(request.mac, mac, sizeof(request.mac));
    request.len = static_cast<std::uint16_t>(len);
    std::memcpy(request.data, data, len);
    request.completionSlot = static_cast<std::uint8_t>(slotIndex);
    request.completionGeneration = generation;
    request.enqueuedMs = millis();
    request.timeoutMs = timeoutMs;
    if (!enqueueRequest(request, priority)) {
        releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
        return false;
    }

    const TickType_t waitTicks = pdMS_TO_TICKS(timeoutMs) > 0U ? pdMS_TO_TICKS(timeoutMs) : 1U;
    if (xSemaphoreTake(g_completionSlots[slotIndex].signal, waitTicks) != pdTRUE) {
        releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
        return false;
    }

    bool callbackReceived = false;
    portENTER_CRITICAL(&g_completionMux);
    const CompletionSlot& slot = g_completionSlots[slotIndex];
    if (slot.inUse && slot.generation == generation) {
        callbackReceived = slot.callbackReceived;
        outDelivered = slot.delivered;
    }
    portEXIT_CRITICAL(&g_completionMux);
    releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
    return callbackReceived;
}

void EspNowManager::peekTxSchedulerCounters(TxSchedulerCounters& out) const {
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        out.enqueued[i] = g_txEnqueued[i];
        out.droppedQueueFull[i] = g_txDroppedQueueFull[i];
    }
    out.driverRejected = g_txDriverRejected;
    out.callbackTimeouts = g_txCallbackTimeouts;
    out.callbacksReceived = g_txCallbacksReceived;
}

// Send to all peers and aggregate delivery status.
bool EspNowManager::sendToAllWithStatus(
    const uint8_t* data,
    size_t len,
    size_t& outDeliveredCount,
    size_t& outTriedCount,
    uint32_t timeoutMs
) const {
    outDeliveredCount = 0;
    outTriedCount = 0;

    if (!initialized_ || deviceCount_ == 0) {
        return false;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        bool delivered = false;
        const bool gotStatus = sendToDeviceWithStatus(i, data, len, delivered, timeoutMs);
        if (!gotStatus) {
            continue;
        }

        ++outTriedCount;
        if (delivered) {
            ++outDeliveredCount;
        }
    }

    return outTriedCount > 0;
}

// Register high-level receive callback.
void EspNowManager::setReceiveCallback(ReceiveCallback callback) {
    receiveCallback_ = callback;
}

// Register high-level send-status callback.
void EspNowManager::setSendCallback(SendCallback callback) {
    sendCallback_ = callback;
}

// Forward the raw ESP-NOW datagram to the instance callback unmodified; this
// class has no protocol knowledge, so bounds are the only thing checked here.
//
// ESP-IDF migration phase 3: arduino-esp32's esp_now_recv_cb_t predated
// esp_now_recv_info_t and carried no RSSI, which is why this used to run a
// whole parallel Wi-Fi promiscuous-mode sniffer (handlePromiscuousRxStatic,
// dot11 header parsing and all) just to recover one. Real ESP-IDF's
// esp_now_recv_info_t::rx_ctrl already has it directly on every callback --
// same field (wifi_pkt_rx_ctrl_t::rssi) the sniffer used to read off a
// second copy of the same over-the-air frame -- so that whole workaround is
// gone, not just adapted; bally_OS's own handleReceiveStatic (BallyRobot.cpp)
// already takes esp_now_recv_info_t the same way.
void EspNowManager::handleReceiveStatic(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
    if (activeInstance_ == nullptr || activeInstance_->receiveCallback_ == nullptr ||
        info == nullptr || info->src_addr == nullptr ||
        incomingData == nullptr || len <= 0 || len > static_cast<int>(MAX_DATA_LEN)) {
        return;
    }

    const uint8_t* mac = info->src_addr;
    const int8_t rssi = (info->rx_ctrl != nullptr) ? static_cast<int8_t>(info->rx_ctrl->rssi) : int8_t(-128);

    const int index = activeInstance_->findDeviceIndexByMac(mac);
    if (index >= 0) {
        activeInstance_->devices_[index].lastRssi = rssi;
    }

    activeInstance_->receiveCallback_(mac, incomingData, static_cast<size_t>(len), rssi);
}

// Dispatch low-level send result to user callback. esp_now_send_info_t (real
// ESP-IDF) carries the peer MAC as des_addr, same role arduino-esp32's bare
// `const uint8_t* mac` parameter used to play directly.
void EspNowManager::handleSendStatic(const esp_now_send_info_t* txInfo, esp_now_send_status_t status) {
    if (activeInstance_ == nullptr) {
        return;
    }

    const uint8_t* mac = (txInfo != nullptr) ? txInfo->des_addr : nullptr;

    if (g_driverStatusQueue != nullptr && mac != nullptr) {
        DriverStatus driverStatus{};
        std::memcpy(driverStatus.mac, mac, sizeof(driverStatus.mac));
        driverStatus.status = status;
        xQueueSend(g_driverStatusQueue, &driverStatus, 0);
    }

    if (activeInstance_->sendCallback_ == nullptr) {
        return;
    }

    activeInstance_->sendCallback_(mac, status);
}

// Add peer in ESP-NOW runtime if not already present.
bool EspNowManager::addPeerToEspNow(const uint8_t mac[6]) const {
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = channel_;
    peerInfo.encrypt = encrypt_;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        return false;
    }

    // Per-peer rate config (see begin()'s comment): real ESP-IDF has no
    // interface-wide equivalent of arduino-esp32's esp_wifi_config_espnow_rate,
    // only esp_now_set_peer_rate_config(). Best-effort -- a failure here
    // just leaves this one peer on the driver's default legacy 802.11b rate,
    // not a reason to fail peer registration outright.
    esp_now_rate_config_t rateConfig = {};
    rateConfig.phymode = WIFI_PHY_MODE_HT20;
    rateConfig.rate = WIFI_PHY_RATE_MCS5_SGI;
    esp_now_set_peer_rate_config(mac, &rateConfig);

    return true;
}

// Remove peer from ESP-NOW runtime table.
bool EspNowManager::removePeerFromEspNow(const uint8_t mac[6]) const {
    if (!esp_now_is_peer_exist(mac)) {
        return true;
    }

    return esp_now_del_peer(mac) == ESP_OK;
}

// Search local device list by MAC address.
int EspNowManager::findDeviceIndexByMac(const uint8_t mac[6]) const {
    if (mac == nullptr) {
        return -1;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        if (memcmp(devices_[i].mac, mac, 6) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

// Safe copy helper for metadata fields.
void EspNowManager::copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}
