/* bluez_adv.c — GDBus implementation of the Jolla-style advertising path:
 * export an org.bluez.LEAdvertisement1 object and register it with the adapter's
 * org.bluez.LEAdvertisingManager1, built from the container's HCI advertising data.
 */
#include "bluez_adv.h"
#include <gio/gio.h>
#include <string.h>

typedef struct { guint16 id; GBytes *data; } Mfg;
typedef struct { char *uuid; GBytes *data; } Svc;

struct BluezAdv {
    GDBusConnection *bus;
    char            *adapter;     /* e.g. /org/bluez/hci1 */
    char            *obj_path;    /* our LEAdvertisement1 object path */
    GDBusNodeInfo   *node;
    guint            reg_id;      /* object registration id (0 = not exported) */
    gboolean         registered;  /* RegisterAdvertisement succeeded */
    /* parsed advertisement fields */
    gboolean         connectable;
    char            *local_name;
    GPtrArray       *uuids;       /* char*  */
    GArray          *mfg;         /* Mfg    */
    GArray          *svc;         /* Svc    */
    gboolean         include_tx;
};

static const char *ADV_XML =
    "<node><interface name='org.bluez.LEAdvertisement1'>"
    "  <method name='Release'/>"
    "  <property name='Type' type='s' access='read'/>"
    "  <property name='ServiceUUIDs' type='as' access='read'/>"
    "  <property name='LocalName' type='s' access='read'/>"
    "  <property name='ManufacturerData' type='a{qv}' access='read'/>"
    "  <property name='ServiceData' type='a{sv}' access='read'/>"
    "  <property name='IncludeTxPower' type='b' access='read'/>"
    "</interface></node>";

/* ---- field helpers ---- */
static char *uuid16_to_str(guint16 u)
{ return g_strdup_printf("0000%04x-0000-1000-8000-00805f9b34fb", u); }
static char *uuid128_to_str(const uint8_t *p /* 16 bytes, little-endian */)
{ return g_strdup_printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        p[15],p[14],p[13],p[12],p[11],p[10],p[9],p[8],p[7],p[6],p[5],p[4],p[3],p[2],p[1],p[0]); }

static void clear_fields(BluezAdv *ba)
{
    g_clear_pointer(&ba->local_name, g_free);
    if (ba->uuids) g_ptr_array_set_size(ba->uuids, 0);
    if (ba->mfg) { for (guint i=0;i<ba->mfg->len;i++) g_bytes_unref(g_array_index(ba->mfg,Mfg,i).data);
                   g_array_set_size(ba->mfg,0); }
    if (ba->svc) { for (guint i=0;i<ba->svc->len;i++){ Svc*s=&g_array_index(ba->svc,Svc,i); g_free(s->uuid); g_bytes_unref(s->data);}
                   g_array_set_size(ba->svc,0); }
    ba->include_tx = FALSE;
}

static void svc_add(BluezAdv *ba, char *uuid, const uint8_t *d, gsize n)
{ Svc s = { uuid ? g_strdup(uuid) : NULL, g_bytes_new(d, n) }; g_array_append_val(ba->svc, s); }
static void mfg_add(BluezAdv *ba, guint16 id, const uint8_t *d, gsize n)
{ Mfg m = { id, g_bytes_new(d, n) }; g_array_append_val(ba->mfg, m); }

/* Parse HCI AD structures: [len][type][value(len-1)]... */
static void parse_ad(BluezAdv *ba, const uint8_t *d, gsize n)
{
    gsize i = 0;
    while (i + 1 < n) {
        uint8_t len = d[i];
        if (len == 0 || i + 1 + len > n) break;
        uint8_t type = d[i+1];
        const uint8_t *v = d + i + 2;
        uint8_t vl = len - 1;
        switch (type) {
        case 0x08: case 0x09:                 /* shortened / complete local name */
            g_free(ba->local_name); ba->local_name = g_strndup((const char*)v, vl); break;
        case 0x02: case 0x03:                 /* 16-bit service UUIDs */
            for (uint8_t k=0;k+1<vl;k+=2) g_ptr_array_add(ba->uuids, uuid16_to_str(v[k]|(v[k+1]<<8)));
            break;
        case 0x06: case 0x07:                 /* 128-bit service UUIDs */
            for (uint8_t k=0;k+15<vl;k+=16) g_ptr_array_add(ba->uuids, uuid128_to_str(v+k));
            break;
        case 0x16: if (vl>=2){ char*u=uuid16_to_str(v[0]|(v[1]<<8)); svc_add(ba,u,v+2,vl-2); g_free(u);} break;
        case 0x21: if (vl>=16){ char*u=uuid128_to_str(v); svc_add(ba,u,v+16,vl-16); g_free(u);} break;
        case 0xFF: if (vl>=2) mfg_add(ba, v[0]|(v[1]<<8), v+2, vl-2); break;
        case 0x0a: ba->include_tx = TRUE; break;   /* TX power present */
        default: break;
        }
        i += 1 + len;
    }
}

/* ---- LEAdvertisement1 D-Bus object ---- */
static void on_method(GDBusConnection *c, const char *sender, const char *path,
                      const char *iface, const char *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer user)
{
    (void)c;(void)sender;(void)path;(void)iface;(void)params;
    if (!g_strcmp0(method, "Release")) {
        BluezAdv *ba = user;
        if (ba) ba->registered = FALSE;   /* BlueZ dropped the advert (e.g. after a connection) */
        g_message("bluez_adv: Release");
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no %s", method);
    }
}

static GVariant *on_get_prop(GDBusConnection *c, const char *sender, const char *path,
                             const char *iface, const char *name, GError **error, gpointer user)
{
    (void)c;(void)sender;(void)path;(void)iface;(void)error;
    BluezAdv *ba = user;
    if (!g_strcmp0(name, "Type"))
        return g_variant_new_string(ba->connectable ? "peripheral" : "broadcast");
    if (!g_strcmp0(name, "LocalName"))
        return g_variant_new_string(ba->local_name ? ba->local_name : "");
    if (!g_strcmp0(name, "IncludeTxPower"))
        return g_variant_new_boolean(ba->include_tx);
    if (!g_strcmp0(name, "ServiceUUIDs")) {
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i=0;i<ba->uuids->len;i++) g_variant_builder_add(&b, "s", ba->uuids->pdata[i]);
        return g_variant_builder_end(&b);
    }
    if (!g_strcmp0(name, "ManufacturerData")) {
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{qv}"));
        for (guint i=0;i<ba->mfg->len;i++) {
            Mfg *m=&g_array_index(ba->mfg,Mfg,i); gsize n; const uint8_t*p=g_bytes_get_data(m->data,&n);
            GVariant *ay=g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,p,n,1);
            g_variant_builder_add(&b, "{qv}", m->id, g_variant_new_variant(ay));
        }
        return g_variant_builder_end(&b);
    }
    if (!g_strcmp0(name, "ServiceData")) {
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        for (guint i=0;i<ba->svc->len;i++) {
            Svc *s=&g_array_index(ba->svc,Svc,i); gsize n; const uint8_t*p=g_bytes_get_data(s->data,&n);
            GVariant *ay=g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,p,n,1);
            g_variant_builder_add(&b, "{sv}", s->uuid, g_variant_new_variant(ay));
        }
        return g_variant_builder_end(&b);
    }
    return NULL;
}

static const GDBusInterfaceVTable VTABLE = { on_method, on_get_prop, NULL, { 0 } };

BluezAdv *bluez_adv_new(const char *adapter_path, GError **error)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (!bus) return NULL;
    BluezAdv *ba = g_new0(BluezAdv, 1);
    ba->bus = bus;
    ba->adapter = g_strdup(adapter_path);
    ba->obj_path = g_strdup("/org/appsupport/advertisement0");
    ba->node = g_dbus_node_info_new_for_xml(ADV_XML, NULL);
    ba->uuids = g_ptr_array_new_with_free_func(g_free);
    ba->mfg = g_array_new(FALSE, FALSE, sizeof(Mfg));
    ba->svc = g_array_new(FALSE, FALSE, sizeof(Svc));
    return ba;
}

static void do_unregister(BluezAdv *ba)
{
    /* Fire-and-forget UnregisterAdvertisement (BlueZ answers DoesNotExist if none). Async so
     * we never block the main loop (which must stay free to serve BlueZ's property reads). */
    g_dbus_connection_call(ba->bus, "org.bluez", ba->adapter,
        "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
        g_variant_new("(o)", ba->obj_path), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
    ba->registered = FALSE;
    if (ba->reg_id) { g_dbus_connection_unregister_object(ba->bus, ba->reg_id); ba->reg_id = 0; }
}

static void on_register_done(GObject *src, GAsyncResult *res, gpointer user)
{
    BluezAdv *ba = user;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) {
        g_variant_unref(r);
        ba->registered = TRUE;
        g_message("bluez_adv: RegisterAdvertisement ok");
    } else if (err && g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)) {
        g_warning("bluez_adv: RegisterAdvertisement denied: %s", err->message);
    } else if (err && strstr(err->message, "AlreadyExists")) {
        ba->registered = TRUE;                 /* already registered == on air */
        g_message("bluez_adv: advertisement already registered (ok)");
    } else {
        g_warning("bluez_adv: RegisterAdvertisement failed: %s", err ? err->message : "?");
    }
    g_clear_error(&err);
}

gboolean bluez_adv_register(BluezAdv *ba, gboolean connectable,
                            const uint8_t *adv, gsize adv_len,
                            const uint8_t *scan_rsp, gsize scan_rsp_len,
                            GError **error)
{
    do_unregister(ba);
    clear_fields(ba);
    ba->connectable = connectable;
    if (adv && adv_len) parse_ad(ba, adv, adv_len);
    if (scan_rsp && scan_rsp_len) parse_ad(ba, scan_rsp, scan_rsp_len);

    ba->reg_id = g_dbus_connection_register_object(ba->bus, ba->obj_path,
                     ba->node->interfaces[0], &VTABLE, ba, NULL, error);
    if (!ba->reg_id) return FALSE;

    /* Async: BlueZ reads this object's properties during RegisterAdvertisement, so the main
     * loop must keep running to answer — a sync call would deadlock (observed as a 5s timeout). */
    GVariantBuilder opts; g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(ba->bus, "org.bluez", ba->adapter,
        "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
        g_variant_new("(oa{sv})", ba->obj_path, &opts),
        NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, on_register_done, ba);
    g_message("bluez_adv: registering (%u uuids, name='%s', connectable=%d)",
              ba->uuids->len, ba->local_name ? ba->local_name : "", connectable);
    return TRUE;
}

void bluez_adv_unregister(BluezAdv *ba) { if (ba) do_unregister(ba); }
gboolean bluez_adv_is_registered(BluezAdv *ba) { return ba && ba->registered; }

void bluez_adv_free(BluezAdv *ba)
{
    if (!ba) return;
    do_unregister(ba);
    clear_fields(ba);
    if (ba->uuids) g_ptr_array_unref(ba->uuids);
    if (ba->mfg) g_array_unref(ba->mfg);
    if (ba->svc) g_array_unref(ba->svc);
    if (ba->node) g_dbus_node_info_unref(ba->node);
    g_free(ba->adapter); g_free(ba->obj_path);
    if (ba->bus) g_object_unref(ba->bus);
    g_free(ba);
}
