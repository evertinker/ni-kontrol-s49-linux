/* Userspace WinUSB for the Kontrol S49 MK3 vendor interface only.
 * Interface 3 is the ODR bulk channel. Interfaces 0 and 1 stay with
 * snd-usb-audio, and interface 2 stays with usbhid. */
#include <errno.h>
#include <stdarg.h>
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT 47653
#define MAGIC 0x53343931u
#define MAX_PAYLOAD (8 * 1024 * 1024)

enum {
    CMD_INIT = 1,
    CMD_GET_DESC = 2,
    CMD_QUERY_IFACE = 3,
    CMD_QUERY_PIPE = 4,
    CMD_READ = 5,
    CMD_WRITE = 6,
    CMD_ABORT = 8
};

struct req {
    uint32_t magic, cmd, a, b, c, n;
};

struct reply {
    int32_t status;
    uint32_t n;
};

struct pipe_reply {
    uint32_t type, id, mps, interval;
};

struct inflight {
    struct inflight *next;
    struct libusb_transfer *xfer;
    uint8_t ep;
};

struct wait_io {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done, status, actual;
};

static libusb_context *usb;
static libusb_device_handle *dev;
static struct libusb_config_descriptor *cfg;
static pthread_mutex_t inf_mu = PTHREAD_MUTEX_INITIALIZER;
static struct inflight *inflight;
static volatile sig_atomic_t running = 1;
static FILE *logf;

static void on_libusb_log(libusb_context *ctx, enum libusb_log_level level, const char *str)
{
    (void)ctx;
    if (!logf || !str)
        return;
    if (level > LIBUSB_LOG_LEVEL_WARNING &&
        !strstr(str, "submiturb") && !strstr(str, "stall") &&
        !strstr(str, "bus error") && !strstr(str, "errno") &&
        !strstr(str, "low-level"))
        return;
    fprintf(logf, "libusb: %s\n", str);
    fflush(logf);
}

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    fputc('\n', logf);
    fflush(logf);
    va_end(ap);
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void track(struct libusb_transfer *xfer, uint8_t ep)
{
    struct inflight *n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->xfer = xfer;
    n->ep = ep;
    pthread_mutex_lock(&inf_mu);
    n->next = inflight;
    inflight = n;
    pthread_mutex_unlock(&inf_mu);
}

static void untrack(struct libusb_transfer *xfer)
{
    pthread_mutex_lock(&inf_mu);
    struct inflight **pp = &inflight;
    while (*pp) {
        if ((*pp)->xfer == xfer) {
            struct inflight *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&inf_mu);
}

static void abort_ep(uint8_t ep)
{
    pthread_mutex_lock(&inf_mu);
    for (struct inflight *n = inflight; n; n = n->next) {
        if (n->ep == ep)
            libusb_cancel_transfer(n->xfer);
    }
    pthread_mutex_unlock(&inf_mu);
}

static void LIBUSB_CALL xfer_done(struct libusb_transfer *xfer)
{
    struct wait_io *io = xfer->user_data;
    int status = xfer->status;
    int actual = xfer->actual_length;
    untrack(xfer);
    libusb_free_transfer(xfer);
    pthread_mutex_lock(&io->mu);
    io->status = status;
    io->actual = actual;
    io->done = 1;
    pthread_cond_signal(&io->cv);
    pthread_mutex_unlock(&io->mu);
}

static const char *xfer_status_name(int status)
{
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED: return "completed";
    case LIBUSB_TRANSFER_ERROR: return "error";
    case LIBUSB_TRANSFER_TIMED_OUT: return "timeout";
    case LIBUSB_TRANSFER_CANCELLED: return "cancelled";
    case LIBUSB_TRANSFER_STALL: return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE: return "no_device";
    case LIBUSB_TRANSFER_OVERFLOW: return "overflow";
    default: return "other";
    }
}

static int bulk_once(uint8_t ep, unsigned char *buf, int len, unsigned timeout,
                     int *actual, int *status)
{
    struct wait_io io;
    memset(&io, 0, sizeof io);
    pthread_mutex_init(&io.mu, NULL);
    pthread_cond_init(&io.cv, NULL);
    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) {
        pthread_cond_destroy(&io.cv);
        pthread_mutex_destroy(&io.mu);
        return LIBUSB_ERROR_NO_MEM;
    }
    libusb_fill_bulk_transfer(xfer, dev, ep, buf, len, xfer_done, &io, timeout);
    track(xfer, ep);
    int rc = libusb_submit_transfer(xfer);
    if (rc) {
        untrack(xfer);
        libusb_free_transfer(xfer);
        pthread_cond_destroy(&io.cv);
        pthread_mutex_destroy(&io.mu);
        log_msg("bulk submit ep=0x%02x len=%d failed: %s", ep, len, libusb_error_name(rc));
        return rc;
    }
    pthread_mutex_lock(&io.mu);
    while (!io.done)
        pthread_cond_wait(&io.cv, &io.mu);
    int st = io.status;
    int act = io.actual;
    pthread_mutex_unlock(&io.mu);
    pthread_cond_destroy(&io.cv);
    pthread_mutex_destroy(&io.mu);
    if (actual)
        *actual = act;
    if (status)
        *status = st;
    if (st == LIBUSB_TRANSFER_COMPLETED)
        return 0;
    if (st == LIBUSB_TRANSFER_TIMED_OUT)
        return LIBUSB_ERROR_TIMEOUT;
    if (st == LIBUSB_TRANSFER_STALL)
        return LIBUSB_ERROR_PIPE;
    if (st == LIBUSB_TRANSFER_CANCELLED)
        return LIBUSB_ERROR_INTERRUPTED;
    return LIBUSB_ERROR_IO;
}

static int bulk(uint8_t ep, unsigned char *buf, int len, unsigned timeout, int *actual)
{
    int status = 0;
    int got = 0;
    int rc = bulk_once(ep, buf, len, timeout, &got, &status);
    int out = (ep & 0x80) == 0;
    if (out || rc)
        log_msg("bulk ep=0x%02x len=%d timeout=%u rc=%d status=%s actual=%d",
                ep, len, timeout, rc, xfer_status_name(status), got);
    /* A halted OUT pipe fails the first host message. Reset the toggle and
     * send the same buffer once more when nothing went out. */
    if (out && (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE) && got == 0) {
        int hc = libusb_clear_halt(dev, ep);
        log_msg("clear halt ep=0x%02x after %s: %s", ep, xfer_status_name(status),
                hc ? libusb_error_name(hc) : "ok");
        if (!hc) {
            status = 0;
            got = 0;
            rc = bulk_once(ep, buf, len, timeout, &got, &status);
            log_msg("bulk retry ep=0x%02x len=%d rc=%d status=%s actual=%d",
                    ep, len, rc, xfer_status_name(status), got);
        }
    } else if (rc == LIBUSB_ERROR_PIPE) {
        libusb_clear_halt(dev, ep);
    }
    if (actual)
        *actual = got;
    return rc;
}

static const struct libusb_interface_descriptor *iface3(void)
{
    if (!cfg)
        return NULL;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            if (iface->altsetting[a].bInterfaceNumber == 3 &&
                iface->altsetting[a].bAlternateSetting == 0)
                return &iface->altsetting[a];
        }
    }
    return NULL;
}

static int open_keyboard(void)
{
    dev = libusb_open_device_with_vid_pid(usb, 0x17cc, 0x2100);
    if (!dev)
        return LIBUSB_ERROR_NO_DEVICE;
    libusb_set_auto_detach_kernel_driver(dev, 0);
    if (libusb_kernel_driver_active(dev, 3) == 1) {
        int det = libusb_detach_kernel_driver(dev, 3);
        log_msg("detached kernel driver on interface 3: %s", libusb_error_name(det));
    }
    int config = -1;
    libusb_get_configuration(dev, &config);
    log_msg("open bus %u port %u config %d",
            libusb_get_bus_number(libusb_get_device(dev)),
            libusb_get_port_number(libusb_get_device(dev)), config);
    int rc = libusb_claim_interface(dev, 3);
    if (rc) {
        log_msg("claim interface 3 failed: %s", libusb_error_name(rc));
        libusb_close(dev);
        dev = NULL;
        return rc;
    }
    if (cfg)
        libusb_free_config_descriptor(cfg);
    cfg = NULL;
    rc = libusb_get_active_config_descriptor(libusb_get_device(dev), &cfg);
    if (rc)
        log_msg("config descriptor: %s", libusb_error_name(rc));
    unsigned char product[128];
    if (libusb_get_string_descriptor_ascii(dev, 2, product, sizeof product) > 0)
        log_msg("claimed interface 3, product %s", product);
    else
        log_msg("claimed interface 3");
    /* WinUSB selects the interface on open. That resets endpoint halt and
     * the data toggle. Without it the keyboard ACKs reads and errors writes. */
    int alt = libusb_set_interface_alt_setting(dev, 3, 0);
    log_msg("set interface 3 alt 0: %s", alt ? libusb_error_name(alt) : "ok");
    static const uint8_t eps[] = { 0x03, 0x83, 0x04 };
    for (size_t i = 0; i < sizeof eps; i++) {
        int hc = libusb_clear_halt(dev, eps[i]);
        log_msg("clear halt ep=0x%02x: %s", eps[i], hc ? libusb_error_name(hc) : "ok");
    }
    return 0;
}

static int get_desc(uint32_t type, uint32_t index, uint32_t lang, unsigned char *buf, int len, int *out)
{
    int rc = libusb_control_transfer(dev,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
        LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)((type << 8) | (index & 0xff)),
        (uint16_t)lang, buf, (uint16_t)len, 1000);
    if (rc < 0)
        return rc;
    *out = rc;
    return 0;
}

static void send_reply(int fd, int status, const void *payload, uint32_t n)
{
    struct reply reply = { status, n };
    if (write_full(fd, &reply, sizeof reply))
        return;
    if (n && payload)
        write_full(fd, payload, n);
}

static void handle_one(int fd)
{
    struct req req;
    if (read_full(fd, &req, sizeof req))
        return;
    if (req.magic != MAGIC || req.n > MAX_PAYLOAD) {
        log_msg("reject cmd=%u n=%u max=%u", req.cmd, req.n, MAX_PAYLOAD);
        /* Read the body before the error reply, or the client sees a reset. */
        if (req.magic == MAGIC && req.n > 0 && req.n <= 32u * 1024u * 1024u) {
            unsigned char buf[8192];
            uint32_t left = req.n;
            while (left) {
                uint32_t chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
                if (read_full(fd, buf, chunk))
                    return;
                left -= chunk;
            }
        }
        send_reply(fd, LIBUSB_ERROR_INVALID_PARAM, NULL, 0);
        return;
    }
    unsigned char *payload = NULL;
    if (req.n) {
        payload = malloc(req.n);
        if (!payload || read_full(fd, payload, req.n)) {
            free(payload);
            return;
        }
    }
    if (!dev) {
        send_reply(fd, LIBUSB_ERROR_NO_DEVICE, NULL, 0);
        free(payload);
        return;
    }
    switch (req.cmd) {
    case CMD_INIT:
        send_reply(fd, 0, NULL, 0);
        break;
    case CMD_GET_DESC: {
        unsigned char desc[4096];
        unsigned cap = (req.c >> 16) & 0xffff;
        if (!cap || cap > sizeof desc)
            cap = sizeof desc;
        int n = 0;
        int rc = get_desc(req.a, req.b, req.c & 0xffff, desc, (int)cap, &n);
        if (rc)
            send_reply(fd, rc, NULL, 0);
        else
            send_reply(fd, 0, desc, (uint32_t)n);
        break;
    }
    case CMD_QUERY_IFACE: {
        const struct libusb_interface_descriptor *d = iface3();
        if (!d || req.a != 0) {
            send_reply(fd, LIBUSB_ERROR_NOT_FOUND, NULL, 0);
            break;
        }
        unsigned char raw[9] = {
            9, LIBUSB_DT_INTERFACE, d->bInterfaceNumber, d->bAlternateSetting,
            d->bNumEndpoints, d->bInterfaceClass, d->bInterfaceSubClass,
            d->bInterfaceProtocol, d->iInterface
        };
        send_reply(fd, 0, raw, sizeof raw);
        break;
    }
    case CMD_QUERY_PIPE: {
        const struct libusb_interface_descriptor *d = iface3();
        if (!d || req.a != 0 || req.b >= d->bNumEndpoints) {
            send_reply(fd, LIBUSB_ERROR_NOT_FOUND, NULL, 0);
            break;
        }
        const struct libusb_endpoint_descriptor *ep = &d->endpoint[req.b];
        struct pipe_reply pipe = {
            ep->bmAttributes & 0x3,
            ep->bEndpointAddress,
            ep->wMaxPacketSize,
            ep->bInterval
        };
        /* WinUSB UsbdPipeTypeBulk is 2. libusb bulk is 2 as well. */
        if ((ep->bmAttributes & 0x3) == LIBUSB_TRANSFER_TYPE_BULK)
            pipe.type = 2;
        else if ((ep->bmAttributes & 0x3) == LIBUSB_TRANSFER_TYPE_INTERRUPT)
            pipe.type = 3;
        send_reply(fd, 0, &pipe, sizeof pipe);
        break;
    }
    case CMD_READ: {
        uint32_t want = req.c;
        if (!want || want > MAX_PAYLOAD) {
            send_reply(fd, LIBUSB_ERROR_INVALID_PARAM, NULL, 0);
            break;
        }
        unsigned char *buf = malloc(want);
        int actual = 0;
        int rc = buf ? bulk((uint8_t)req.a, buf, (int)want, req.b, &actual) : LIBUSB_ERROR_NO_MEM;
        if (rc || actual < 0)
            send_reply(fd, rc ? rc : LIBUSB_ERROR_IO, NULL, 0);
        else
            send_reply(fd, 0, buf, (uint32_t)actual);
        free(buf);
        break;
    }
    case CMD_WRITE: {
        int actual = 0;
        int rc = bulk((uint8_t)req.a, payload ? payload : (unsigned char *)"", (int)req.n, req.b, &actual);
        /* n is the accepted byte count. No payload follows; the DLL must not read one. */
        struct reply reply = { rc, rc ? 0u : (uint32_t)actual };
        write_full(fd, &reply, sizeof reply);
        break;
    }
    case CMD_ABORT:
        abort_ep((uint8_t)req.a);
        send_reply(fd, 0, NULL, 0);
        break;
    default:
        send_reply(fd, LIBUSB_ERROR_NOT_SUPPORTED, NULL, 0);
        break;
    }
    free(payload);
}

static void *client_thread(void *arg)
{
    int fd = *(int *)arg;
    free(arg);
    handle_one(fd);
    close(fd);
    return NULL;
}

static void *event_thread(void *arg)
{
    (void)arg;
    while (running) {
        struct timeval tv = { 1, 0 };
        libusb_handle_events_timeout(usb, &tv);
    }
    return NULL;
}

int main(void)
{
    logf = fopen("/tmp/ni-s49-usb.log", "a");
    if (!logf)
        return 1;
    setvbuf(logf, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    int rc = libusb_init(&usb);
    if (rc) {
        log_msg("libusb_init %s", libusb_error_name(rc));
        return 1;
    }
    libusb_set_option(usb, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    libusb_set_log_cb(usb, on_libusb_log, LIBUSB_LOG_CB_CONTEXT);
    while (running) {
        rc = open_keyboard();
        if (!rc)
            break;
        log_msg("keyboard not claimable yet: %s", libusb_error_name(rc));
        sleep(2);
    }
    if (!running)
        return 1;
    pthread_t events;
    pthread_create(&events, NULL, event_thread, NULL);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) || listen(srv, 16)) {
        log_msg("listen 127.0.0.1:%d failed: %s", PORT, strerror(errno));
        return 1;
    }
    log_msg("listening on 127.0.0.1:%d", PORT);
    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        int *slot = malloc(sizeof *slot);
        *slot = fd;
        pthread_t th;
        pthread_create(&th, NULL, client_thread, slot);
        pthread_detach(th);
    }
    if (dev) {
        libusb_release_interface(dev, 3);
        libusb_close(dev);
    }
    libusb_exit(usb);
    return 0;
}
