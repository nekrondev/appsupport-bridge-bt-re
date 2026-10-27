/* gbinder_svc.c — container-facing HIDL service.
 *
 * Serves android.hardware.bluetooth@1.0::IBluetoothHci to the Android container over
 * hwbinder (/dev/hwpuddlejumper), exactly as the stock bridge registers it. Container
 * commands are forwarded to the controller (hci0); controller packets are delivered back
 * via the IBluetoothHciCallbacks the container passed to initialize().
 *
 * HIDL method/callback transaction codes (observed from the stock bridge + the HIDL ABI):
 *   IBluetoothHci:          1 initialize(cb)  2 sendHciCommand  3 sendAclData  4 sendScoData  5 close
 *   IBluetoothHciCallbacks: 1 initializationComplete(Status)  2 hciEventReceived
 *                           3 aclDataReceived  4 scoDataReceived
 *
 * NOTE (stage 2): every controller packet is delivered to the container. The container/BlueZ
 * demux and per-opcode virtualization arrive in stages 3–4 (see DESIGN.md).
 */
#include "bridge.h"
#include "mux.h"
#include <gbinder.h>
#include <string.h>

#define HWBINDER_DEV "/dev/hwpuddlejumper"
#define IFACE_HCI    "android.hardware.bluetooth@1.0::IBluetoothHci"
#define IFACE_CB     "android.hardware.bluetooth@1.0::IBluetoothHciCallbacks"

#define BT_STATUS_SUCCESS 0

/* ---- controller <- container : submit a command/data packet through the mux ---- */
static void forward_to_controller(App *app, uint8_t h4type,
                                  const uint8_t *payload, gsize plen)
{
    if (!payload) return;
    uint8_t *pkt = g_malloc(plen + 1);
    pkt[0] = h4type;
    if (plen) memcpy(pkt + 1, payload, plen);
    mux_submit(app, ORIG_CONTAINER, pkt, plen + 1);
    g_free(pkt);
}

/* ---- IBluetoothHci transaction handler (container -> bridge) ---- */
static void on_cb_reply(GBinderClient *client, GBinderRemoteReply *reply, int status, void *user)
{
    (void)client; (void)reply; (void)user;
    g_message("  callback transact completed: status=%d", status);
}

static GBinderLocalReply *on_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                      guint code, guint flags, int *status, void *user)
{
    App *app = user;
    GBinderReader reader;
    gbinder_remote_request_init_reader(req, &reader);
    *status = GBINDER_STATUS_OK;
    g_message("container txn: code=%u flags=0x%x iface=%s", code, flags,
              gbinder_remote_request_interface(req));

    switch (code) {
    case 1: { /* initialize(IBluetoothHciCallbacks callback) */
        GBinderRemoteObject *cb = gbinder_reader_read_object(&reader);
        if (cb) {
            if (app->cb_client)  gbinder_client_unref(app->cb_client);
            if (app->cb_obj)     gbinder_remote_object_unref(app->cb_obj);
            app->cb_obj    = gbinder_remote_object_ref(cb);
            app->cb_client = gbinder_client_new(app->cb_obj, IFACE_CB);
            g_message("container initialize(): callbacks=%p", (void*)cb);
            /* reply to the container: initializationComplete(SUCCESS) */
            GBinderLocalRequest *r = gbinder_client_new_request(app->cb_client);
            GBinderWriter w; gbinder_local_request_init_writer(r, &w);
            gbinder_writer_append_int32(&w, BT_STATUS_SUCCESS);
            gbinder_client_transact(app->cb_client, 1 /*initializationComplete*/, 0,
                                    r, on_cb_reply, NULL, NULL);
            gbinder_local_request_unref(r);
            g_message("  sent initializationComplete(SUCCESS)");
        } else {
            g_warning("initialize(): no callbacks object");
        }
        break;
    }
    case 2:   /* sendHciCommand(vec<uint8>) */
    case 3:   /* sendAclData(vec<uint8>)    */
    case 4: { /* sendScoData(vec<uint8>)    */
        gsize count = 0;
        const guint8 *data = gbinder_reader_read_hidl_byte_vec(&reader, &count);
        uint8_t h4 = (code == 2) ? H4_CMD : (code == 3) ? H4_ACL : H4_SCO;
        if (data) forward_to_controller(app, h4, data, count);
        break;
    }
    case 5:   /* close() */
        g_message("container close()");
        if (app->cb_client) { gbinder_client_unref(app->cb_client); app->cb_client = NULL; }
        if (app->cb_obj)    { gbinder_remote_object_unref(app->cb_obj); app->cb_obj = NULL; }
        break;
    default:
        /* gbinder handles the HIDL base meta-transactions; anything else: empty reply */
        break;
    }

    if (flags & GBINDER_TX_FLAG_ONEWAY) return NULL;
    /* HIDL reply framing: every non-oneway method returns at least _hidl_err (Status=OK).
     * Without it the container's proxy can't parse the reply and treats the call as failed. */
    GBinderLocalReply *reply = gbinder_local_object_new_reply(obj);
    gbinder_local_reply_append_int32(reply, 0 /* _hidl_err = OK */);
    return reply;
}

/* ---- controller -> container : deliver event/acl/sco up to the callbacks ---- */
void gbinder_svc_deliver(App *app, const uint8_t *buf, gsize len)
{
    if (!app->cb_client || len < 1) return;
    guint code;
    switch (buf[0]) {
    case H4_EVT: code = 2; break;   /* hciEventReceived */
    case H4_ACL: code = 3; break;   /* aclDataReceived  */
    case H4_SCO: code = 4; break;   /* scoDataReceived  */
    default: return;
    }
    GBinderLocalRequest *r = gbinder_client_new_request(app->cb_client);
    GBinderWriter w; gbinder_local_request_init_writer(r, &w);
    gbinder_writer_append_hidl_vec(&w, buf + 1, (guint)(len - 1), 1);
    gbinder_client_transact(app->cb_client, code, 0, r, NULL, NULL, NULL);
    gbinder_local_request_unref(r);
}

/* ---- publish / presence ---- */
static void on_add_service(GBinderServiceManager *sm, int status, void *user)
{
    (void)sm; (void)user;
    if (status == 0) g_message("published %s/default", IFACE_HCI);
    else             g_warning("add_service(%s/default) failed: %d", IFACE_HCI, status);
}

static void publish_if_ready(App *app)
{
    if (app->published || !app->sm) return;
    app->hci_obj = gbinder_servicemanager_new_local_object(app->sm, IFACE_HCI, on_transact, app);
    if (!app->hci_obj) { g_warning("failed to create local object"); return; }
    gbinder_servicemanager_add_service(app->sm, "default", app->hci_obj, on_add_service, app);
    app->published = TRUE;
    g_message("registering %s/default on %s", IFACE_HCI, HWBINDER_DEV);
}

static void on_sm_presence(GBinderServiceManager *sm, void *user)
{
    App *app = user;
    if (gbinder_servicemanager_is_present(sm)) publish_if_ready(app);
}

gboolean gbinder_svc_start(App *app)
{
    app->sm = gbinder_servicemanager_new(HWBINDER_DEV);
    if (!app->sm) { g_warning("gbinder_servicemanager_new(%s) failed", HWBINDER_DEV); return FALSE; }
    app->sm_presence_id =
        gbinder_servicemanager_add_presence_handler(app->sm, on_sm_presence, app);
    if (gbinder_servicemanager_is_present(app->sm)) publish_if_ready(app);
    else g_message("waiting for %s to appear", HWBINDER_DEV);
    return TRUE;
}

void gbinder_svc_stop(App *app)
{
    if (app->cb_client) gbinder_client_unref(app->cb_client);
    if (app->cb_obj)    gbinder_remote_object_unref(app->cb_obj);
    if (app->hci_obj)   gbinder_local_object_unref(app->hci_obj);
    if (app->sm_presence_id) gbinder_servicemanager_remove_handler(app->sm, app->sm_presence_id);
    if (app->sm)        gbinder_servicemanager_unref(app->sm);
    app->cb_client = NULL; app->cb_obj = NULL; app->hci_obj = NULL; app->sm = NULL;
}
