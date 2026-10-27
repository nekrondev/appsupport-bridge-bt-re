/* config.c — read /etc/appsupport-bridge-bt.conf and apply per-model quirks.
 *
 * Mirrors Jolla's BrokenExtendedLECreateConnectionModels handling: for a device model
 * listed there, LE Extended Create Connection is converted to the legacy form (done in
 * mux.c, gated on app->quirk_ext_conn_to_legacy).
 */
#include "config.h"
#include <string.h>

#define CONF "/etc/appsupport-bridge-bt.conf"

/* device model as reported inside the container (ro.product.model) */
static char *get_model(void)
{
    char *out = NULL;
    int status = 0;
    const char *cmd = "lxc-attach -n defaultuser -P /tmp/appsupport -- getprop ro.product.model";
    if (g_spawn_command_line_sync(cmd, &out, NULL, &status, NULL) && out) {
        g_strstrip(out);
        if (*out) return out;
        g_free(out);
    }
    return NULL;
}

void config_load(App *app)
{
    app->quirk_ext_conn_to_legacy = FALSE;

    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, CONF, G_KEY_FILE_NONE, NULL)) {
        char *list = g_key_file_get_string(kf, "Bluetooth",
                                           "BrokenExtendedLECreateConnectionModels", NULL);
        if (list && *list) {
            char *model = get_model();
            if (model) {
                gchar **items = g_strsplit(list, ",", -1);
                for (int i = 0; items[i]; i++) {
                    if (strcmp(g_strstrip(items[i]), model) == 0) {
                        app->quirk_ext_conn_to_legacy = TRUE;
                        break;
                    }
                }
                g_strfreev(items);
                g_message("config: model='%s' ext-conn->legacy quirk=%d",
                          model, app->quirk_ext_conn_to_legacy);
                g_free(model);
            }
        }
        g_free(list);
    } else {
        g_message("config: %s not found, using defaults", CONF);
    }
    g_key_file_free(kf);
}
