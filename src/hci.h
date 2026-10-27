/* hci.h — exclusive HCI_CHANNEL_USER access to the real controller (hci0).
 * Reverse-engineered from appsupport-bridge-bt setup path. */
#ifndef BRIDGE_HCI_H
#define BRIDGE_HCI_H

#include <glib.h>
#include <stdint.h>

typedef struct HciChannel HciChannel;

/* Called for every packet read from the controller (type byte included:
 * 0x02=ACL, 0x03=SCO, 0x04=event). buf/len exclude nothing; buf[0] is the type. */
typedef void (*HciRxFunc)(const uint8_t *buf, gsize len, void *user);

/* Open exclusive user-channel on hci<dev_id> (brings the device down first, as the
 * stock bridge does), install a glib io-watch, and route inbound packets to rx. */
HciChannel *hci_channel_open(int dev_id, HciRxFunc rx, void *user, GError **error);

/* Write a raw HCI packet to the controller (buf[0] = packet type). */
gboolean hci_channel_write(HciChannel *ch, const uint8_t *buf, gsize len, GError **error);

void hci_channel_close(HciChannel *ch);

#endif
