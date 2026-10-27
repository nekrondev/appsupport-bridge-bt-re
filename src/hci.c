/* hci.c — controller-side HCI channel (hci0, HCI_CHANNEL_USER).
 *
 * Mirrors the stock bridge's setup:
 *   fd = socket(AF_BLUETOOTH, SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC, BTPROTO_HCI)
 *   ioctl(fd, HCIDEVDOWN, dev)                 // take the device down to claim it
 *   bind(fd, sockaddr_hci{dev, HCI_CHANNEL_USER})
 *   g_io_add_watch(G_IO_IN|HUP|ERR) -> read packets
 */
#include "hci.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI      1
#define HCI_CHANNEL_USER 1
/* HCIDEVDOWN = _IOW('H', 202, int) == 0x400448CA (matches the RE'd immediate) */
#define HCIDEVDOWN 0x400448CA

struct sockaddr_hci {
    unsigned short hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct HciChannel {
    int fd;
    GIOChannel *io;
    guint watch;
    HciRxFunc rx;
    void *user;
};

#define HCI_MAX_FRAME 1500   /* HCI_MAX_FRAME_SIZE; stock bridge caps at 0x5dc=1500 */

static gboolean on_io(GIOChannel *src, GIOCondition cond, gpointer data)
{
    HciChannel *ch = data;
    (void)src;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        g_warning("hci0: io channel error/hup (0x%x)", cond);
        return G_SOURCE_CONTINUE; /* keep watch; controller may recover on reset */
    }
    uint8_t buf[HCI_MAX_FRAME];
    for (;;) {
        ssize_t n = read(ch->fd, buf, sizeof buf);
        if (n > 0) {
            if (ch->rx) ch->rx(buf, (gsize)n, ch->user);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            g_warning("hci0: read error: %s", g_strerror(errno));
            break;
        } else {
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

HciChannel *hci_channel_open(int dev_id, HciRxFunc rx, void *user, GError **error)
{
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "socket(AF_BLUETOOTH): %s", g_strerror(errno));
        return NULL;
    }
    /* Claim the device: bring it down so we can bind the user channel exclusively. */
    if (ioctl(fd, HCIDEVDOWN, dev_id) < 0 && errno != ENODEV && errno != EALREADY) {
        /* Non-fatal per stock behaviour unless bind fails; log and continue. */
        g_debug("hci%d: HCIDEVDOWN: %s", dev_id, g_strerror(errno));
    }
    struct sockaddr_hci a = {
        .hci_family  = AF_BLUETOOTH,
        .hci_dev     = (unsigned short)dev_id,
        .hci_channel = HCI_CHANNEL_USER,
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "bind(hci%d, HCI_CHANNEL_USER): %s", dev_id, g_strerror(errno));
        close(fd);
        return NULL;
    }
    HciChannel *ch = g_new0(HciChannel, 1);
    ch->fd = fd; ch->rx = rx; ch->user = user;
    ch->io = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(ch->io, NULL, NULL);
    g_io_channel_set_buffered(ch->io, FALSE);
    g_io_channel_set_close_on_unref(ch->io, TRUE);
    ch->watch = g_io_add_watch(ch->io, G_IO_IN | G_IO_HUP | G_IO_ERR, on_io, ch);
    g_message("hci%d: bound HCI_CHANNEL_USER, fd=%d", dev_id, fd);
    return ch;
}

gboolean hci_channel_write(HciChannel *ch, const uint8_t *buf, gsize len, GError **error)
{
    gsize off = 0;
    while (off < len) {
        ssize_t n = write(ch->fd, buf + off, len - off);
        if (n > 0) { off += (gsize)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "hci write: %s", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

void hci_channel_close(HciChannel *ch)
{
    if (!ch) return;
    if (ch->watch) g_source_remove(ch->watch);
    if (ch->io) g_io_channel_unref(ch->io);
    g_free(ch);
}
