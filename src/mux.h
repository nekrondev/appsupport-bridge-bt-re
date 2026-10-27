/* mux.h — shared-controller HCI multiplexer.
 *
 * One controller (hci0) is shared by two consumers: the Android container and host
 * BlueZ. Commands from both are serialized under the controller's command-credit flow;
 * each Command Complete/Status is routed back ONLY to the consumer that issued the
 * matching command. (Async-event ownership routing is refined in a later stage.)
 */
#ifndef BRIDGE_MUX_H
#define BRIDGE_MUX_H

#include "bridge.h"

typedef enum { ORIG_CONTAINER = 0, ORIG_BLUEZ = 1, ORIG_INTERNAL = 2 } Origin;

void mux_init(App *app);
void mux_free(App *app);

/* Submit an H4 packet (buf[0] = type) from an origin toward the controller.
 * Commands (0x01) are credit-queued; ACL/SCO are forwarded immediately. */
void mux_submit(App *app, Origin origin, const uint8_t *buf, gsize len);

/* A packet arrived from the controller (buf[0] = type): route it. */
void mux_on_event(App *app, const uint8_t *buf, gsize len);

#endif
