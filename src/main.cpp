#include <Arduino.h>
#include <AppRuntime.h>

namespace {
AppRuntime g_app;
}

// arduino-esp32's main.cpp creates the loop() task with this size via a
// weak symbol it leaves overridable (framework-arduinoespressif32/cores/
// esp32/main.cpp), default 8192. AppRuntime::tick() drains the RX-routed
// queues on this same task/stack (AppRuntime.cpp's flushPendingEspNowOutput
// -> EspNowConfig::drainRoutedQueues), and that path nests, worst case,
// ProtocolRouter::RoutedMessage (~672 B, drainOneQueue's by-value copy) +
// handleLogItem's char text[617] + SerialMux::forwardRelay's
// frameBytes[kMaxFrameBytes=2088] + enqueueFrameBytes's QueuedFrame{}
// (~2096 B, value-initialized) -- over 5 KB of the 8 KB default already,
// before the console/BTP-terminal shell (TinyShell/ShellLineEditor) or
// DatabaseStore's SQLite calls that also run every tick add their own
// frames on top. Same class of stack-overflow-during-radio-traffic risk
// AppRuntime::startEspNowWorkers()/startHeartbeatWorker() already raised
// their own task stacks for (10240/8192, up from smaller originals) --
// this is the one task neither of those covers.
size_t getArduinoLoopTaskStackSize(void) {
    return 16384;
}

void setup() {
    g_app.begin();
}

void loop() {
    g_app.tick();
}
