/* vhci.h — virtual HCI device (/dev/vhci) for host BlueZ to attach to (hci1).
 * The bridge is the "controller" for this virtual device: it reads packets the host
 * stack sends down, and writes packets up to the host stack. RE'd from the stock bridge. */
#ifndef BRIDGE_VHCI_H
#define BRIDGE_VHCI_H

#include <glib.h>
#include <stdint.h>

typedef struct VhciChannel VhciChannel;

/* rx: host-stack packet travelling DOWN to the virtual controller (buf[0] = H4 type). */
typedef void (*VhciRxFunc)(const uint8_t *buf, gsize len, void *user);

/* Create an HCI_PRIMARY virtual device; host BlueZ auto-attaches (becomes hci1). */
VhciChannel *vhci_open(VhciRxFunc rx, void *user, GError **error);

/* Inject a packet UP to the host stack (buf[0] = H4 type), as if from the controller. */
gboolean vhci_write(VhciChannel *v, const uint8_t *buf, gsize len, GError **error);

int  vhci_index(VhciChannel *v);
void vhci_close(VhciChannel *v);

#endif
