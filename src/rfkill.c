/* rfkill.c — Bluetooth rfkill coordination.
 *
 * The container shares the controller with host BlueZ. If the Bluetooth rfkill switch is
 * soft-blocked (e.g. host UI toggling BT), the shared controller would go down and break
 * AppSupport. We watch /dev/rfkill and re-unblock Bluetooth soft-blocks to keep the radio
 * available (matching Jolla's "unblocking hci1 / keep BT up for AppSupport"). Hard blocks
 * (physical kill switch) are left alone.
 */
#include "rfkill.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define RFKILL_TYPE_BLUETOOTH 2
#define RFKILL_OP_CHANGE      2

struct rfk_evt {            /* classic 8-byte rfkill_event (newer kernels append fields) */
    guint32 idx;
    guint8  type;
    guint8  op;
    guint8  soft;
    guint8  hard;
} __attribute__((packed));

typedef struct { int fd; GIOChannel *io; guint watch; App *app; } RfkillMon;

static gboolean on_rfk(GIOChannel *src, GIOCondition cond, gpointer data)
{
    RfkillMon *m = data;
    (void)src;
    if (cond & (G_IO_HUP | G_IO_ERR)) return G_SOURCE_CONTINUE;
    unsigned char buf[64];
    for (;;) {
        ssize_t n = read(m->fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if ((size_t)n < sizeof(struct rfk_evt)) continue;
        struct rfk_evt *e = (struct rfk_evt *)buf;
        if (e->type != RFKILL_TYPE_BLUETOOTH) continue;
        g_message("rfkill: idx=%u op=%u soft=%u hard=%u", e->idx, e->op, e->soft, e->hard);
        if (e->soft && !e->hard) {
            struct rfk_evt u = { .idx = e->idx, .type = RFKILL_TYPE_BLUETOOTH,
                                 .op = RFKILL_OP_CHANGE, .soft = 0, .hard = 0 };
            if (write(m->fd, &u, sizeof u) == (ssize_t)sizeof u)
                g_message("rfkill: re-unblocked idx=%u (keep BT up for AppSupport)", e->idx);
        }
    }
    return G_SOURCE_CONTINUE;
}

void rfkill_start(App *app)
{
    int fd = open("/dev/rfkill", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { g_warning("rfkill: open: %s", g_strerror(errno)); return; }
    RfkillMon *m = g_new0(RfkillMon, 1);
    m->fd = fd; m->app = app;
    m->io = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(m->io, NULL, NULL);
    g_io_channel_set_buffered(m->io, FALSE);
    g_io_channel_set_close_on_unref(m->io, TRUE);
    m->watch = g_io_add_watch(m->io, G_IO_IN | G_IO_HUP | G_IO_ERR, on_rfk, m);
    app->rfkill = m;
    g_message("rfkill: monitoring /dev/rfkill");
}

void rfkill_stop(App *app)
{
    RfkillMon *m = app->rfkill;
    if (!m) return;
    if (m->watch) g_source_remove(m->watch);
    if (m->io) g_io_channel_unref(m->io);
    g_free(m);
    app->rfkill = NULL;
}
