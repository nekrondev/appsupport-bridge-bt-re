/* vhci.c — /dev/vhci virtual controller for host BlueZ.
 *
 * Mirrors the stock bridge:
 *   fd = open("/dev/vhci", O_RDWR|O_CLOEXEC)
 *   write(fd, {HCI_VENDOR_PKT=0xff, HCI_PRIMARY=0x00}, 2)   // create the device
 *   poll(fd, POLLIN, 500ms)                                 // await the create response
 *   read(fd) -> create response (type + index)
 * Afterwards read(fd) yields host packets going down (to transmit), and write(fd)
 * injects packets up to the host stack.
 */
#include "vhci.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#define HCI_VENDOR_PKT 0xff
#define HCI_PRIMARY    0x00
#define VHCI_MAX_FRAME 1500

struct VhciChannel {
    int fd;
    int index;
    GIOChannel *io;
    guint watch;
    VhciRxFunc rx;
    void *user;
};

static gboolean on_io(GIOChannel *src, GIOCondition cond, gpointer data)
{
    VhciChannel *v = data;
    (void)src;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        g_warning("vhci: io error/hup (0x%x)", cond);
        return G_SOURCE_CONTINUE;
    }
    uint8_t buf[VHCI_MAX_FRAME];
    for (;;) {
        ssize_t n = read(v->fd, buf, sizeof buf);
        if (n > 0) { if (v->rx) v->rx(buf, (gsize)n, v->user); }
        else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            g_warning("vhci: read error: %s", g_strerror(errno));
            break;
        } else break;
    }
    return G_SOURCE_CONTINUE;
}

VhciChannel *vhci_open(VhciRxFunc rx, void *user, GError **error)
{
    int fd = open("/dev/vhci", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "open(/dev/vhci): %s", g_strerror(errno));
        return NULL;
    }
    const uint8_t create[2] = { HCI_VENDOR_PKT, HCI_PRIMARY };
    if (write(fd, create, sizeof create) != (ssize_t)sizeof create) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "vhci create write: %s", g_strerror(errno));
        close(fd);
        return NULL;
    }

    int index = -1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
        uint8_t resp[16];
        ssize_t n = read(fd, resp, sizeof resp);
        /* Kernel create-response: {HCI_VENDOR_PKT, opcode/type, ...}; the device index
         * is reported in the trailing bytes on kernels that include it. Log best-effort. */
        if (n >= 2) {
            if (n >= 3) index = resp[2];
            g_message("vhci create response: type=0x%02x index=%d (len=%zd)",
                      resp[1], index, n);
        }
    } else {
        g_message("vhci create response not provided immediately");
    }

    /* non-blocking for the glib io-watch */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    VhciChannel *v = g_new0(VhciChannel, 1);
    v->fd = fd; v->index = index; v->rx = rx; v->user = user;
    v->io = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(v->io, NULL, NULL);
    g_io_channel_set_buffered(v->io, FALSE);
    g_io_channel_set_close_on_unref(v->io, TRUE);
    v->watch = g_io_add_watch(v->io, G_IO_IN | G_IO_HUP | G_IO_ERR, on_io, v);
    g_message("vhci: created (hci%d), fd=%d", index, fd);
    return v;
}

gboolean vhci_write(VhciChannel *v, const uint8_t *buf, gsize len, GError **error)
{
    gsize off = 0;
    while (off < len) {
        ssize_t n = write(v->fd, buf + off, len - off);
        if (n > 0) { off += (gsize)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "vhci write: %s", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

int vhci_index(VhciChannel *v) { return v ? v->index : -1; }

void vhci_close(VhciChannel *v)
{
    if (!v) return;
    if (v->watch) g_source_remove(v->watch);
    if (v->io) g_io_channel_unref(v->io);
    g_free(v);
}
