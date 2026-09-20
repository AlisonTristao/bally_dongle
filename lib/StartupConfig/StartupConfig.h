#pragma once

#include <DonglePeripherals.h>

#include "compat.h"

namespace StartupConfig{

/**
 * @brief Initializes LED/LCD and announces boot status. Does not wait for a
 * serial terminal/monitor to attach -- see the definition for why that wait
 * was removed.
 *
 * @param io Console transport for the "startup: iniciando..." banner line
 * (phase 6: ConsoleCdc; there is no Arduino global Serial anymore).
 *
 * No interactive prompt follows: the clock is queried/corrected by the BTP
 * client via the "dongle clock" / "dongle set_clock" shell commands.
 */
void announceBoot(DonglePeripherals& peripherals, ByteIO& io);

} // namespace StartupConfig
