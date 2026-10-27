/* main.c — appsupport-bridge-bt reimplementation (Design A).
 *
 * Stage 1: glib main loop + exclusive controller channel (hci0).
 * Stage 2: gbinder IBluetoothHci service to the container; controller packets are
 *          delivered up to the container's callbacks.
 * Later  : vhci + BlueZ relay (stage 3), HCI mux + advertising fix (stage 4),
 *          rfkill/quirks (stage 5).  See DESIGN.md.
 */
#include <glib.h>
#include <glib-unix.h>
#include <stdlib.h>
#include "bridge.h"
#include "mux.h"
#include "config.h"
#include "rfkill.h"

/* Packet from the controller (buf[0] = H4 type): the mux routes it to the consumer
 * that issued the matching command (and updates command credits). */
static void on_controller_rx(const uint8_t *buf, gsize len, void *user)
{
    mux_on_event((App *)user, buf, len);
}

/* Packet from host BlueZ (down through the vhci, buf[0] = H4 type) -> mux -> controller. */
static void on_vhci_rx(const uint8_t *buf, gsize len, void *user)
{
    mux_submit((App *)user, ORIG_BLUEZ, buf, len);
}

static gboolean on_signal(gpointer data)
{
    App *app = data;
    g_message("signal received, shutting down");
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    int dev_id = 0;                             /* hci0 = real controller */
    if (argc > 1) dev_id = atoi(argv[1]);

    App app;
    memset(&app, 0, sizeof app);
    mux_init(&app);
    config_load(&app);
    app.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT,  on_signal, &app);
    g_unix_signal_add(SIGTERM, on_signal, &app);

    GError *err = NULL;
    app.hci0 = hci_channel_open(dev_id, on_controller_rx, &app, &err);
    if (!app.hci0) {
        g_printerr("fatal: %s\n", err ? err->message : "hci_channel_open failed");
        g_clear_error(&err);
        return 1;
    }

    /* Virtual controller for host BlueZ (hci1). Non-fatal if it fails: the container
     * path still works, we just lose host-side coexistence. */
    app.vhci = vhci_open(on_vhci_rx, &app, &err);
    if (!app.vhci) {
        g_warning("vhci unavailable: %s (host BlueZ coexistence disabled)",
                  err ? err->message : "?");
        g_clear_error(&err);
    }
    /* Adapter BlueZ manages (the vhci) — where we register AppSupport advertisements. */
    app.bluez_adapter = g_strdup_printf("/org/bluez/hci%d",
                            (app.vhci && vhci_index(app.vhci) >= 0) ? vhci_index(app.vhci) : 1);

    if (!gbinder_svc_start(&app)) {
        g_printerr("fatal: could not start gbinder service\n");
        return 1;
    }

    rfkill_start(&app);   /* keep BT available for AppSupport across rfkill toggles */

    g_message("appsupport-bridge-bt (reimpl) running on hci%d", dev_id);
    g_main_loop_run(app.loop);

    rfkill_stop(&app);
    gbinder_svc_stop(&app);
    vhci_close(app.vhci);
    hci_channel_close(app.hci0);
    mux_free(&app);
    g_free(app.bluez_adapter);
    g_main_loop_unref(app.loop);
    return 0;
}
