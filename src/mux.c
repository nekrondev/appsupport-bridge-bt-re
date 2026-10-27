/* mux.c — shared-controller HCI multiplexer + coexistence virtualization.
 *
 * Transport: commands from the container and host BlueZ are serialized under the
 * controller's command-credit flow; Command Complete/Status are routed back to the
 * issuing consumer; async events are routed by activity ownership.
 *
 * Virtualization (so the two stacks don't corrupt each other's controller state):
 *   - LE scan enable/disable arbitration (legacy 0x200c / extended 0x2042): the
 *     controller scan stays on while *either* consumer wants it; advertising reports
 *     go to every consumer currently scanning.
 *   - Resolving list (0x2027/28/29) and Filter-Accept list (0x2011/12/10): reference-
 *     counted per consumer; duplicate add / still-referenced remove return a synthetic
 *     success without touching the controller; clear removes only the caller's entries.
 */
#include "mux.h"
#include "bluez_adv.h"
#include <string.h>
#include <stdlib.h>

static int g_dbg = -1;
static inline int dbg(void) { if (g_dbg < 0) g_dbg = getenv("ABBR_DEBUG") ? 1 : 0; return g_dbg; }
#define DBG(...) do { if (dbg()) g_message(__VA_ARGS__); } while (0)
static const char *onm(Origin o) { return o == ORIG_CONTAINER ? "APP" : o == ORIG_BLUEZ ? "BZ" : "INT"; }

typedef struct { Origin origin; uint16_t opcode; uint16_t orig_opcode; uint8_t *pkt; gsize len; } CmdEntry;

static void cmd_free(gpointer p) { CmdEntry *e = p; if (e) { g_free(e->pkt); g_free(e); } }

void mux_init(App *app)
{
    app->cmd_pending  = g_queue_new();
    app->cmd_inflight = g_queue_new();
    app->cmd_credits  = 1;
    app->adv_owner = app->scan_owner = app->initiator = -1;
    app->conn_owner = g_hash_table_new(NULL, NULL);
    app->scan_want[0] = app->scan_want[1] = 0;
    app->scan_on = FALSE;
    app->reslist = g_hash_table_new(NULL, NULL);
    app->acclist = g_hash_table_new(NULL, NULL);
}

void mux_free(App *app)
{
    if (app->cmd_pending)  { g_queue_free_full(app->cmd_pending,  cmd_free); app->cmd_pending  = NULL; }
    if (app->cmd_inflight) { g_queue_free_full(app->cmd_inflight, cmd_free); app->cmd_inflight = NULL; }
    if (app->conn_owner)   { g_hash_table_destroy(app->conn_owner); app->conn_owner = NULL; }
    if (app->reslist)      { g_hash_table_destroy(app->reslist); app->reslist = NULL; }
    if (app->acclist)      { g_hash_table_destroy(app->acclist); app->acclist = NULL; }
    if (app->badv)         { bluez_adv_free(app->badv); app->badv = NULL; }
}

/* ---- delivery ---- */
static void deliver(App *app, Origin o, const uint8_t *buf, gsize len)
{
    if (o == ORIG_CONTAINER) {
        gbinder_svc_deliver(app, buf, len);
    } else if (o == ORIG_BLUEZ && app->vhci) {
        GError *err = NULL;
        if (!vhci_write(app->vhci, buf, len, &err)) {
            g_warning("deliver->BlueZ: %s", err ? err->message : "?");
            g_clear_error(&err);
        }
    }
    /* ORIG_INTERNAL: nothing to deliver to */
}
static void deliver_both(App *app, const uint8_t *buf, gsize len)
{
    deliver(app, ORIG_CONTAINER, buf, len);
    deliver(app, ORIG_BLUEZ, buf, len);
}

/* synthesize a Command Complete (status + optional extra return params) to one consumer */
static void synth_cc_ret(App *app, Origin o, uint16_t op, uint8_t status,
                         const uint8_t *ret, uint8_t retn)
{
    uint8_t e[32];
    if (retn > sizeof e - 7) retn = 0;
    e[0]=H4_EVT; e[1]=0x0e; e[2]=(uint8_t)(4+retn); e[3]=0x01;
    e[4]=(uint8_t)(op&0xff); e[5]=(uint8_t)(op>>8); e[6]=status;
    if (retn) memcpy(e+7, ret, retn);
    DBG("SYNTH CC op=0x%04x status=0x%02x -> %s", op, status, onm(o));
    deliver(app, o, e, 7u+retn);
}
static void synth_cc(App *app, Origin o, uint16_t op, uint8_t status)
{ synth_cc_ret(app, o, op, status, NULL, 0); }

/* ---- Jolla-style advertising: container advert -> host BlueZ LEAdvertisingManager1 ---- */
static void adv_start(App *app)
{
    if (!app->badv) {
        GError *e = NULL;
        app->badv = bluez_adv_new(app->bluez_adapter ? app->bluez_adapter : "/org/bluez/hci1", &e);
        if (!app->badv) { g_warning("bluez_adv_new: %s", e?e->message:"?"); g_clear_error(&e); return; }
    }
    GError *e = NULL;
    if (bluez_adv_register(app->badv, app->adv_connectable,
                           app->adv_data, app->adv_data_len,
                           app->scan_rsp, app->scan_rsp_len, &e))
        app->adv_for_container = TRUE;
    else { g_warning("RegisterAdvertisement failed: %s", e?e->message:"?"); g_clear_error(&e); }
}
static void adv_stop(App *app)
{
    if (app->badv) bluez_adv_unregister(app->badv);
    app->adv_for_container = FALSE;
}

/* Intercept the container's extended-advertising commands (do NOT forward to the controller);
 * register the advertisement with BlueZ instead. Returns TRUE if handled. */
static gboolean virt_adv(App *app, Origin o, uint16_t op, const uint8_t *buf, gsize len)
{
    if (o != ORIG_CONTAINER) return FALSE;
    switch (op) {
    case 0x2036:                                  /* Set Extended Advertising Parameters */
        if (len >= 7) app->adv_connectable = (buf[5] & 0x01) != 0;
        { uint8_t tx = 0x00; synth_cc_ret(app, o, op, 0x00, &tx, 1); } /* status + selected_tx_power */
        return TRUE;
    case 0x2037:                                  /* Set Extended Advertising Data */
        if (len >= 8) { gsize dl = buf[7]; if (dl > sizeof app->adv_data) dl = sizeof app->adv_data;
                        if (len >= 8 + dl) { memcpy(app->adv_data, buf + 8, dl); app->adv_data_len = dl; } }
        synth_cc(app, o, op, 0x00); return TRUE;
    case 0x2038:                                  /* Set Extended Scan Response Data */
        if (len >= 8) { gsize dl = buf[7]; if (dl > sizeof app->scan_rsp) dl = sizeof app->scan_rsp;
                        if (len >= 8 + dl) { memcpy(app->scan_rsp, buf + 8, dl); app->scan_rsp_len = dl; } }
        synth_cc(app, o, op, 0x00); return TRUE;
    case 0x2039:                                  /* Set Extended Advertising Enable */
        if (len >= 5 && buf[4]) adv_start(app); else adv_stop(app);
        synth_cc(app, o, op, 0x00); return TRUE;
    case 0x203c: case 0x203d:                     /* Remove / Clear Advertising Set(s) */
        adv_stop(app); synth_cc(app, o, op, 0x00); return TRUE;
    default: return FALSE;
    }
}

/* ---- connection-handle ownership ---- */
static void conn_set(App *app, guint h, int o) { g_hash_table_insert(app->conn_owner, GUINT_TO_POINTER(h), GINT_TO_POINTER(o + 1)); }
static int  conn_get(App *app, guint h) { gpointer v = g_hash_table_lookup(app->conn_owner, GUINT_TO_POINTER(h)); return v ? GPOINTER_TO_INT(v) - 1 : -1; }
static void conn_del(App *app, guint h) { g_hash_table_remove(app->conn_owner, GUINT_TO_POINTER(h)); }

/* ---- command flow ---- */
static void try_send(App *app)
{
    while (app->cmd_credits > 0 && !g_queue_is_empty(app->cmd_pending)) {
        CmdEntry *e = g_queue_pop_head(app->cmd_pending);
        GError *err = NULL;
        if (!hci_channel_write(app->hci0, e->pkt, e->len, &err)) {
            g_warning("hci0 cmd write: %s", err ? err->message : "?");
            g_clear_error(&err); cmd_free(e); continue;
        }
        DBG("CMD->ctrl op=0x%04x (%s)", e->opcode, onm(e->origin));
        g_queue_push_tail(app->cmd_inflight, e);
        app->cmd_credits--;
    }
}

static void enqueue_cmd_o(App *app, Origin origin, const uint8_t *buf, gsize len, uint16_t orig_op)
{
    CmdEntry *e = g_new0(CmdEntry, 1);
    e->origin = origin; e->opcode = buf[1] | (buf[2] << 8); e->orig_opcode = orig_op;
    e->pkt = g_memdup2(buf, len); e->len = len;
    g_queue_push_tail(app->cmd_pending, e);
    try_send(app);
}
static void enqueue_cmd(App *app, Origin origin, const uint8_t *buf, gsize len)
{ enqueue_cmd_o(app, origin, buf, len, buf[1] | (buf[2] << 8)); }

/* Per-model quirk: convert LE Extended Create Connection (0x2043, 1M PHY) to legacy
 * LE Create Connection (0x200d). Returns TRUE and fills out/outlen (caller frees). */
static gboolean convert_ext_conn_to_legacy(const uint8_t *b, gsize len, uint8_t **out, gsize *outlen)
{
    if (len < 4 + 14) return FALSE;
    uint8_t ifp = b[4], own = b[5], pat = b[6];
    const uint8_t *pa = b + 7;
    uint8_t phys = b[13];
    if (!(phys & 0x01)) return FALSE;            /* need a 1M PHY block to map */
    if (len < 14 + 16) return FALSE;
    const uint8_t *blk = b + 14;                 /* first (1M) PHY block, 16 bytes */
    uint8_t pkt[4 + 25];
    pkt[0] = H4_CMD; pkt[1] = 0x0d; pkt[2] = 0x20; pkt[3] = 25;
    uint8_t *p = pkt + 4;
    p[0] = blk[0]; p[1] = blk[1];                /* scan_interval */
    p[2] = blk[2]; p[3] = blk[3];                /* scan_window   */
    p[4] = ifp; p[5] = pat;
    memcpy(p + 6, pa, 6);                        /* peer_addr      */
    p[12] = own;
    memcpy(p + 13, blk + 4, 12);                 /* conn interval/latency/timeout/ce */
    *out = g_memdup2(pkt, sizeof pkt); *outlen = sizeof pkt;
    return TRUE;
}

/* inject a self-originated command (its completion is swallowed) */
static void inject_cmd(App *app, const uint8_t *buf, gsize len) { enqueue_cmd(app, ORIG_INTERNAL, buf, len); }

/* ---- address-keyed list virtualization (resolving list / accept list) ---- */
static guint64 addr_key(const uint8_t *p /* ->addr_type,addr[6] */)
{
    guint64 a = 0;
    for (int i = 0; i < 6; i++) a |= (guint64)p[1 + i] << (8 * i);
    return ((guint64)p[0] << 48) | a;
}

/* returns TRUE if fully handled (synthetic reply sent, do not forward) */
static gboolean virt_list_add(App *app, Origin o, uint16_t op, GHashTable *t,
                              const uint8_t *buf, gsize len)
{
    if (len < 11) return FALSE;
    guint64 k = addr_key(buf + 4);
    int mask = GPOINTER_TO_INT(g_hash_table_lookup(t, GSIZE_TO_POINTER(k)));
    gboolean on_ctrl = (mask != 0);
    g_hash_table_insert(t, GSIZE_TO_POINTER(k), GINT_TO_POINTER(mask | (1 << o)));
    if (on_ctrl) { synth_cc(app, o, op, 0x00); return TRUE; }  /* already on controller */
    return FALSE;                                              /* first add: forward */
}

static gboolean virt_list_remove(App *app, Origin o, uint16_t op, GHashTable *t,
                                 const uint8_t *buf, gsize len)
{
    if (len < 11) return FALSE;
    guint64 k = addr_key(buf + 4);
    int mask = GPOINTER_TO_INT(g_hash_table_lookup(t, GSIZE_TO_POINTER(k)));
    if (mask == 0) { synth_cc(app, o, op, 0x00); return TRUE; } /* not present: lenient success */
    int nm = mask & ~(1 << o);
    if (nm == 0) { g_hash_table_remove(t, GSIZE_TO_POINTER(k)); return FALSE; } /* forward real remove */
    g_hash_table_insert(t, GSIZE_TO_POINTER(k), GINT_TO_POINTER(nm));
    synth_cc(app, o, op, 0x00);                                 /* still referenced by other */
    return TRUE;
}

static gboolean virt_list_clear(App *app, Origin o, uint16_t op, GHashTable *t,
                                uint16_t remove_op)
{
    GHashTableIter it; gpointer key, val;
    GList *to_remove = NULL;     /* keys to drop from controller (origin was sole owner) */
    GSList *to_update = NULL;    /* keys still owned by others: (key,newmask) */
    gboolean other_owns = FALSE;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        int mask = GPOINTER_TO_INT(val);
        int nm = mask & ~(1 << o);
        if (nm != 0) other_owns = TRUE;
        if (nm != mask) { /* origin owned this entry */
            if (nm == 0) to_remove = g_list_prepend(to_remove, key);
            else { to_update = g_slist_prepend(to_update, key);
                   to_update = g_slist_prepend(to_update, GINT_TO_POINTER(nm)); }
        }
    }
    if (!other_owns) {               /* nobody else owns anything: real clear is safe */
        g_hash_table_remove_all(t);
        g_list_free(to_remove); g_slist_free(to_update);
        return FALSE;                /* forward the clear */
    }
    /* Remove only this origin's sole-owned entries from the controller, keep the rest. */
    for (GList *l = to_remove; l; l = l->next) {
        guint64 k = (guint64)GPOINTER_TO_SIZE(l->data); /* key stored via GUINT_TO_POINTER */
        uint8_t at = (uint8_t)(k >> 48);
        uint8_t pkt[11] = { H4_CMD, (uint8_t)(remove_op & 0xff), (uint8_t)(remove_op >> 8), 0x07,
                            at, (uint8_t)k, (uint8_t)(k>>8), (uint8_t)(k>>16),
                            (uint8_t)(k>>24), (uint8_t)(k>>32), (uint8_t)(k>>40) };
        inject_cmd(app, pkt, sizeof pkt);
        g_hash_table_remove(t, l->data);
    }
    for (GSList *s = to_update; s; s = s->next->next) {
        int nm = GPOINTER_TO_INT(s->data);
        gpointer k = s->next->data;
        g_hash_table_insert(t, k, GINT_TO_POINTER(nm));
    }
    g_list_free(to_remove); g_slist_free(to_update);
    synth_cc(app, o, op, 0x00);
    return TRUE;
}

/* LE scan enable/disable arbitration; enable byte @buf[4] for both 0x200c and 0x2042 */
static gboolean virt_scan_enable(App *app, Origin o, uint16_t op, const uint8_t *buf, gsize len)
{
    if (len < 5) return FALSE;
    int enable = buf[4] ? 1 : 0;
    int other  = (o == ORIG_CONTAINER) ? ORIG_BLUEZ : ORIG_CONTAINER;
    app->scan_want[o] = enable;
    if (enable) app->scan_type[o] = (op == 0x2042) ? 2 : 1;  /* extended vs legacy scan */
    if (enable) {
        if (app->scan_on) { synth_cc(app, o, op, 0x00); return TRUE; } /* already scanning */
        app->scan_on = TRUE; app->scan_owner = o;
        return FALSE;                                                  /* forward: turn on */
    } else {
        if (app->scan_want[other]) { synth_cc(app, o, op, 0x00); return TRUE; } /* keep on */
        app->scan_on = FALSE;
        return FALSE;                                                  /* forward: turn off */
    }
}

/* returns TRUE if the command was fully virtualized (do not forward) */
static gboolean virt_handle_command(App *app, Origin o, const uint8_t *buf, gsize len)
{
    uint16_t op = buf[1] | (buf[2] << 8);
    switch (op) {
    case 0x200c: case 0x2042: return virt_scan_enable(app, o, op, buf, len);
    case 0x2011: return virt_list_add   (app, o, op, app->acclist, buf, len);
    case 0x2027: return virt_list_add   (app, o, op, app->reslist, buf, len);
    case 0x2012: return virt_list_remove(app, o, op, app->acclist, buf, len);
    case 0x2028: return virt_list_remove(app, o, op, app->reslist, buf, len);
    case 0x2010: return virt_list_clear (app, o, op, app->acclist, 0x2012);
    case 0x2029: return virt_list_clear (app, o, op, app->reslist, 0x2028);
    default: return FALSE;
    }
}

/* note ownership-changing commands */
static void track_command(App *app, Origin o, uint16_t op, const uint8_t *buf, gsize len)
{
    switch (op) {
    case 0x200a: case 0x2039: if (len >= 5 && buf[4]) app->adv_owner = o; break;
    case 0x200d: case 0x2043: app->initiator = o; break;
    default: break;
    }
}

void mux_submit(App *app, Origin origin, const uint8_t *buf, gsize len)
{
    if (len < 1) return;
    if (buf[0] == H4_CMD) {
        if (len < 3) return;
        if (origin != ORIG_INTERNAL && virt_handle_command(app, origin, buf, len))
            return;                                  /* handled via virtualization */
        uint16_t op = buf[1] | (buf[2] << 8);
        if (virt_adv(app, origin, op, buf, len)) return;   /* container advertising -> BlueZ */
        if (origin == ORIG_CONTAINER && op == 0x2043 && app->quirk_ext_conn_to_legacy) {
            uint8_t *leg = NULL; gsize ll = 0;
            if (convert_ext_conn_to_legacy(buf, len, &leg, &ll)) {
                DBG("quirk: converting 0x2043 -> 0x200d for container");
                track_command(app, origin, 0x200d, leg, ll);
                enqueue_cmd_o(app, origin, leg, ll, 0x2043 /* report completion as 0x2043 */);
                g_free(leg);
                return;
            }
        }
        DBG("CMD<-%s op=0x%04x len=%zu", onm(origin), op, (size_t)len);
        track_command(app, origin, op, buf, len);
        enqueue_cmd(app, origin, buf, len);
    } else {
        GError *err = NULL;
        if (!hci_channel_write(app->hci0, buf, len, &err)) {
            g_warning("hci0 data write: %s", err ? err->message : "?");
            g_clear_error(&err);
        }
    }
}

static gboolean pop_inflight(App *app, uint16_t opcode, Origin *out, uint16_t *orig_out)
{
    for (GList *l = app->cmd_inflight->head; l; l = l->next) {
        CmdEntry *e = l->data;
        if (e->opcode == opcode) {
            *out = e->origin; *orig_out = e->orig_opcode;
            g_queue_delete_link(app->cmd_inflight, l);
            cmd_free(e);
            return TRUE;
        }
    }
    return FALSE;
}

/* route a completed command's event to its issuer (swallow internal injections) */
static void route_completion(App *app, uint16_t op, const uint8_t *buf, gsize len)
{
    if (op == 0x0000) return;
    Origin o; uint16_t orig = op;
    if (!pop_inflight(app, op, &o, &orig)) { deliver_both(app, buf, len); return; }
    if (o == ORIG_INTERNAL) return;                       /* swallow self-injected */
    if (orig != op) {    /* quirk-converted: report completion under the original opcode */
        uint8_t *m = g_memdup2(buf, len);
        if      (m[1] == 0x0e && len >= 6) { m[4] = orig & 0xff; m[5] = orig >> 8; }
        else if (m[1] == 0x0f && len >= 7) { m[5] = orig & 0xff; m[6] = orig >> 8; }
        deliver(app, o, m, len);
        g_free(m);
        return;
    }
    /* Hide controller-wide advertising filter (APCF) support from the container, so it
     * uses software filtering instead of fighting BlueZ over the shared hardware filters.
     * LE Get Vendor Capabilities (0xFD53) return params: ...,[+6]filtering_support,[+7]max_filter */
    if (o == ORIG_CONTAINER && op == 0xFD53 && len >= 14) {
        uint8_t *m = g_memdup2(buf, len);
        m[12] = 0;    /* filtering_support = 0 */
        m[13] = 0;    /* max_filter       = 0 */
        DBG("hid vendor APCF filter caps from container");
        deliver(app, o, m, len);
        g_free(m);
        return;
    }
    deliver(app, o, buf, len);
}

/* Convert a single-report LE Extended Advertising Report (subevent 0x0d) into a legacy
 * LE Advertising Report (0x02) for consumers doing a legacy scan.
 * Returns: 1 = converted (out/outlen set, caller frees); 0 = drop (non-legacy PDU);
 *          -1 = pass the original through unchanged. */
static int ext_rep_to_legacy(const uint8_t *b, gsize len, uint8_t **out, gsize *outlen)
{
    if (len < 29 || b[4] != 1) return -1;               /* only single-report conversion */
    uint16_t et = b[5] | (b[6] << 8);
    if (!(et & 0x10)) return 0;                         /* not a legacy PDU -> drop */
    uint8_t at = b[7];
    const uint8_t *addr = b + 8;
    uint8_t rssi = b[18];
    uint8_t dlen = b[28];
    if (len < (gsize)29 + dlen) return -1;
    const uint8_t *data = b + 29;

    uint8_t legt;                                       /* legacy event_type */
    if (et & 0x08)        legt = 0x04;                  /* SCAN_RSP */
    else if ((et & 0x05) == 0x05) legt = 0x01;         /* connectable+directed -> ADV_DIRECT_IND */
    else if ((et & 0x03) == 0x03) legt = 0x00;         /* connectable+scannable -> ADV_IND */
    else if (et & 0x02)   legt = 0x02;                 /* scannable -> ADV_SCAN_IND */
    else                  legt = 0x03;                 /* -> ADV_NONCONN_IND */

    gsize ol = 4 /*hdr: 04 3e len subevent*/ + 1 /*num*/ + 1 /*evt*/ + 1 /*at*/ + 6 + 1 /*dlen*/ + dlen + 1 /*rssi*/;
    uint8_t *o = g_malloc(ol);
    o[0] = H4_EVT; o[1] = 0x3e; o[2] = (uint8_t)(ol - 3); o[3] = 0x02; /* LE Advertising Report */
    o[4] = 1;                      /* num_reports */
    o[5] = legt; o[6] = at;
    memcpy(o + 7, addr, 6);
    o[13] = dlen;
    if (dlen) memcpy(o + 14, data, dlen);
    o[14 + dlen] = rssi;
    *out = o; *outlen = ol;
    return 1;
}

/* deliver an advertising report to every consumer currently scanning, converting an
 * extended report to legacy for a consumer that is doing a legacy scan. */
static void deliver_scanners(App *app, const uint8_t *buf, gsize len)
{
    gboolean any = FALSE;
    gboolean is_ext = (len >= 4 && buf[1] == 0x3e && buf[3] == 0x0d);
    for (int o = 0; o < 2; o++) {
        if (!app->scan_want[o]) continue;
        any = TRUE;
        if (is_ext && app->scan_type[o] == 1) {        /* legacy scanner, extended report */
            uint8_t *leg = NULL; gsize ol = 0;
            int r = ext_rep_to_legacy(buf, len, &leg, &ol);
            if (r == 1)      { deliver(app, o, leg, ol); g_free(leg); }
            else if (r < 0)  { deliver(app, o, buf, len); }
            /* r == 0: non-legacy PDU, drop for this consumer */
        } else {
            deliver(app, o, buf, len);
        }
    }
    if (!any) deliver_both(app, buf, len);
}

static void route_async(App *app, const uint8_t *buf, gsize len)
{
    if (buf[0] == H4_ACL) {
        if (len >= 3) { int o = conn_get(app, buf[1] | ((buf[2] & 0x0f) << 8));
                        if (o >= 0) { deliver(app, o, buf, len); return; } }
        deliver_both(app, buf, len); return;
    }
    if (buf[0] != H4_EVT) { deliver_both(app, buf, len); return; }

    switch (buf[1]) {
    case 0x05: /* Disconnection Complete: handle @buf[4..5] */
        if (len >= 6) { guint h = buf[4] | (buf[5] << 8); int o = conn_get(app, h);
                        if (o >= 0) { deliver(app, o, buf, len); conn_del(app, h); return; } }
        deliver_both(app, buf, len); return;
    case 0x13: /* Number of Completed Packets: first handle @buf[4..5] */
        if (len >= 6) { int o = conn_get(app, buf[4] | (buf[5] << 8));
                        if (o >= 0) { deliver(app, o, buf, len); return; } }
        deliver_both(app, buf, len); return;
    case 0x3e: { /* LE Meta */
        uint8_t sub = buf[3];
        if ((sub == 0x01 || sub == 0x0a) && len >= 8) { /* (Enhanced) Connection Complete */
            uint8_t status = buf[4]; guint h = buf[5] | (buf[6] << 8); uint8_t role = buf[7];
            int owner = (role == 0x01) ? app->adv_owner : app->initiator;
            /* A peripheral connection from a BlueZ-registered AppSupport advert belongs to the
             * container (its GATT server answers), even though BlueZ drove the advertising. */
            if (role == 0x01 && app->adv_for_container) owner = ORIG_CONTAINER;
            if (status == 0x00 && owner >= 0) conn_set(app, h, owner);
            if (owner >= 0) { deliver(app, owner, buf, len); return; }
        } else if (sub == 0x02 || sub == 0x0d) {        /* (Extended) Advertising Report */
            deliver_scanners(app, buf, len); return;
        } else if (len >= 7 && (sub == 0x03 || sub == 0x04 || sub == 0x05 ||
                                sub == 0x07 || sub == 0x0c)) {
            int o = conn_get(app, buf[5] | (buf[6] << 8));
            if (o >= 0) { deliver(app, o, buf, len); return; }
        }
        deliver_both(app, buf, len); return;
    }
    default:
        deliver_both(app, buf, len); return;
    }
}

void mux_on_event(App *app, const uint8_t *buf, gsize len)
{
    if (len < 2) return;
    if (dbg() && buf[0] == H4_EVT) g_message("EVT<-ctrl ev=0x%02x len=%zu", buf[1], (size_t)len);

    if (buf[0] == H4_EVT && buf[1] == 0x0e && len >= 6) {         /* Command Complete */
        app->cmd_credits = buf[3];
        route_completion(app, buf[4] | (buf[5] << 8), buf, len);
        try_send(app);
        return;
    }
    if (buf[0] == H4_EVT && buf[1] == 0x0f && len >= 7) {         /* Command Status */
        app->cmd_credits = buf[4];
        route_completion(app, buf[5] | (buf[6] << 8), buf, len);
        try_send(app);
        return;
    }
    route_async(app, buf, len);
}
