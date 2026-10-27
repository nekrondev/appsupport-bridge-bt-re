/* bridge.h — shared context for the appsupport-bridge-bt reimplementation. */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <glib.h>
#include <gbinder.h>
#include <stdint.h>
#include "hci.h"
#include "vhci.h"

typedef struct App {
    GMainLoop            *loop;
    HciChannel           *hci0;          /* exclusive user-channel to the controller */
    VhciChannel          *vhci;          /* virtual device host BlueZ attaches to (hci1) */

    /* container-facing gbinder service (IBluetoothHci) */
    GBinderServiceManager *sm;
    gulong                 sm_presence_id;
    GBinderLocalObject    *hci_obj;      /* our IBluetoothHci local object        */
    GBinderRemoteObject   *cb_obj;       /* IBluetoothHciCallbacks from initialize */
    GBinderClient         *cb_client;
    gboolean               published;

    /* shared-controller command multiplexer (mux.c) */
    GQueue                *cmd_pending;  /* CmdEntry* awaiting a command credit */
    GQueue                *cmd_inflight; /* CmdEntry* sent, awaiting completion  */
    int                    cmd_credits;  /* HCI command credits (controller-reported) */

    /* ownership routing for async events (mux.c); Origin value, or -1 = none */
    int                    adv_owner;    /* who last enabled advertising */
    int                    scan_owner;   /* who last enabled scanning    */
    int                    initiator;    /* who last issued create-connection */
    GHashTable            *conn_owner;   /* conn handle (guint) -> Origin+1 */

    /* coexistence virtualization (mux.c) */
    int                    scan_want[2]; /* per-origin desired LE scan enable */
    int                    scan_type[2]; /* per-origin scan kind: 0 none, 1 legacy, 2 extended */
    gboolean               scan_on;      /* controller actual scan state */
    GHashTable            *reslist;      /* resolving list:   addr key(guint64) -> owner mask */
    GHashTable            *acclist;      /* filter-accept list: addr key(guint64) -> owner mask */

    /* per-model quirks from /etc/appsupport-bridge-bt.conf (config.c) */
    gboolean               quirk_ext_conn_to_legacy; /* convert 0x2043 -> 0x200d for the container */

    /* Jolla-style advertising: the container's advertising is routed to host BlueZ via
     * LEAdvertisingManager1 (bluez_adv.c) instead of driven on the controller directly. */
    char                  *bluez_adapter;   /* /org/bluez/hciN (the vhci BlueZ manages) */
    struct BluezAdv       *badv;            /* lazily created on first advertise */
    gboolean               adv_for_container;/* an AppSupport advert is registered with BlueZ */
    gboolean               adv_connectable;  /* from LE Set Ext Adv Params event properties */
    uint8_t                adv_data[256];    gsize adv_data_len;
    uint8_t                scan_rsp[256];    gsize scan_rsp_len;

    /* rfkill coordination (rfkill.c) */
    void                  *rfkill;       /* opaque RfkillMon* */
} App;

/* HCI H4 packet types (first byte on the user channel). */
#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_SCO   0x03
#define H4_EVT   0x04

/* container side (gbinder_svc.c) */
gboolean gbinder_svc_start(App *app);
void     gbinder_svc_stop(App *app);
/* deliver a controller packet (buf[0] = H4 type) up to the container callbacks */
void     gbinder_svc_deliver(App *app, const uint8_t *buf, gsize len);

#endif
