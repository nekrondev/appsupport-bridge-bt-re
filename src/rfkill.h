/* rfkill.h — keep the Bluetooth controller available for AppSupport by watching
 * /dev/rfkill and re-unblocking Bluetooth soft-blocks. */
#ifndef BRIDGE_RFKILL_H
#define BRIDGE_RFKILL_H
#include "bridge.h"
void rfkill_start(App *app);
void rfkill_stop(App *app);
#endif
