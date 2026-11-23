/* Native winusb.dll for the NI prefix. Wine 11.22's builtin returns
 * failure from every call. Transfers go to the local helper, which
 * owns USB interface 3 of the S49 MK3. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <winusb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct dev {
    LONG timeouts[256];
    LONG partial[256];
};

static CRITICAL_SECTION log_mu;
static int log_ready;

static void log_line(const char *fmt, ...)
{
    if (!log_ready)
        return;
    FILE *f = fopen("Z:\\tmp\\ni-s49-winusb.log", "a");
    if (!f)
        return;
    EnterCriticalSection(&log_mu);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    LeaveCriticalSection(&log_mu);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&log_mu);
        log_ready = 1;
        log_line("winusb.dll loaded");
    }
    return TRUE;
}

static int map_err(int status)
{
    if (status == -7)
        return ERROR_SEM_TIMEOUT;
    if (status == -9)
        return ERROR_BAD_COMMAND;
    if (status == -4)
        return ERROR_DEVICE_NOT_CONNECTED;
    return ERROR_GEN_FAILURE;
}

static SOCKET connect_helper(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int call(uint32_t cmd, uint32_t a, uint32_t b, uint32_t c,
                const void *in, uint32_t in_n, void *out, uint32_t out_cap, uint32_t *out_n)
{
    SOCKET s = connect_helper();
    if (s == INVALID_SOCKET)
        return -1;
    struct req req = { MAGIC, cmd, a, b, c, in_n };
    if (send(s, (const char *)&req, sizeof req, 0) != (int)sizeof req) {
        closesocket(s);
        return -1;
    }
    uint32_t sent = 0;
    const char *ip = in;
    while (sent < in_n) {
        int w = send(s, ip + sent, (int)(in_n - sent), 0);
        if (w <= 0) {
            closesocket(s);
            return -1;
        }
        sent += (uint32_t)w;
    }
    struct reply reply;
    int got = 0;
    while (got < (int)sizeof reply) {
        int r = recv(s, (char *)&reply + got, (int)sizeof reply - got, 0);
        if (r <= 0) {
            closesocket(s);
            return -1;
        }
        got += r;
    }
    uint32_t n = reply.n;
    if (n > MAX_PAYLOAD) {
        closesocket(s);
        return -1;
    }
    /* Reads and descriptors put `n` bytes after the header. A write's `n` is
     * only the number of bytes accepted; the helper sends no body. Reading
     * it back hits EOF and was reported as a failed write. */
    if (out && n) {
        uint32_t have = 0;
        char scratch[1024];
        while (have < n) {
            uint32_t chunk = n - have;
            char *dst;
            if (have < out_cap) {
                dst = (char *)out + have;
                if (chunk > out_cap - have)
                    chunk = out_cap - have;
            } else {
                dst = scratch;
                if (chunk > sizeof scratch)
                    chunk = (uint32_t)sizeof scratch;
            }
            int r = recv(s, dst, (int)chunk, 0);
            if (r <= 0) {
                closesocket(s);
                return -1;
            }
            have += (uint32_t)r;
        }
    }
    closesocket(s);
    if (out_n)
        *out_n = (out && reply.n > out_cap) ? out_cap : reply.n;
    return reply.status;
}

static struct dev *dev_of(WINUSB_INTERFACE_HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return NULL;
    return h;
}

__declspec(dllexport) BOOL WINAPI WinUsb_Initialize(HANDLE device, PWINUSB_INTERFACE_HANDLE iface)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    log_line("Initialize device=%p", device);
    if (!iface) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    int rc = call(CMD_INIT, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    if (rc) {
        log_line("Initialize helper status %d", rc);
        SetLastError(rc < 0 ? (DWORD)map_err(rc) : ERROR_GEN_FAILURE);
        return FALSE;
    }
    struct dev *dev = calloc(1, sizeof *dev);
    if (!dev) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    *iface = dev;
    log_line("Initialize ok %p", dev);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE iface)
{
    struct dev *dev = dev_of(iface);
    if (!dev) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    free(dev);
    log_line("Free");
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE iface,
        UCHAR alt, PUSB_INTERFACE_DESCRIPTOR desc)
{
    if (!dev_of(iface) || !desc) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    uint32_t n = 0;
    int rc = call(CMD_QUERY_IFACE, alt, 0, 0, NULL, 0, desc, sizeof *desc, &n);
    log_line("QueryInterfaceSettings alt=%u rc=%d n=%u if=%u eps=%u class=%u sub=%u",
            alt, rc, n, desc->bInterfaceNumber, desc->bNumEndpoints,
            desc->bInterfaceClass, desc->bInterfaceSubClass);
    if (rc || n < sizeof *desc) {
        SetLastError(rc ? (DWORD)map_err(rc) : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE iface,
        UCHAR alt, UCHAR index, PWINUSB_PIPE_INFORMATION info)
{
    if (!dev_of(iface) || !info) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    struct pipe_reply pipe;
    uint32_t n = 0;
    int rc = call(CMD_QUERY_PIPE, alt, index, 0, NULL, 0, &pipe, sizeof pipe, &n);
    if (rc || n < sizeof pipe) {
        log_line("QueryPipe alt=%u index=%u rc=%d", alt, index, rc);
        SetLastError(rc ? (DWORD)map_err(rc) : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(info, 0, sizeof *info);
    info->PipeType = (USBD_PIPE_TYPE)pipe.type;
    info->PipeId = (UCHAR)pipe.id;
    info->MaximumPacketSize = (USHORT)pipe.mps;
    info->Interval = (UCHAR)pipe.interval;
    log_line("QueryPipe index=%u id=0x%x type=%u mps=%u", index, info->PipeId, pipe.type, pipe.mps);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE iface,
        UCHAR type, UCHAR index, USHORT lang, PUCHAR buf, ULONG len, PULONG transferred)
{
    if (!dev_of(iface) || !buf) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    uint32_t n = 0;
    uint32_t packed = (uint32_t)lang | ((len > 0xffff ? 0xffff : len) << 16);
    int rc = call(CMD_GET_DESC, type, index, packed, NULL, 0, buf, len, &n);
    log_line("GetDescriptor type=%u index=%u lang=%u rc=%d n=%u", type, index, lang, rc, n);
    if (rc) {
        SetLastError(map_err(rc));
        return FALSE;
    }
    if (transferred)
        *transferred = n;
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE iface,
        UCHAR pipe, ULONG policy, ULONG value_len, PVOID value)
{
    struct dev *dev = dev_of(iface);
    if (!dev) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    ULONG dword = 0;
    if (value && value_len >= sizeof(ULONG))
        memcpy(&dword, value, sizeof dword);
    if (policy == PIPE_TRANSFER_TIMEOUT)
        dev->timeouts[pipe] = (LONG)dword;
    if (policy == ALLOW_PARTIAL_READS)
        dev->partial[pipe] = (LONG)dword;
    log_line("SetPipePolicy pipe=0x%x policy=%lu value=%lu", pipe, policy, dword);
    return TRUE;
}

static BOOL transfer(struct dev *dev, int write, UCHAR pipe, PUCHAR buf, ULONG len,
        PULONG transferred, LPOVERLAPPED ov)
{
    ULONG timeout = 0;
    if (dev->timeouts[pipe] > 0)
        timeout = (ULONG)dev->timeouts[pipe];
    uint32_t n = 0;
    int rc = call(write ? CMD_WRITE : CMD_READ, pipe, timeout, write ? 0 : len,
            write ? buf : NULL, write ? len : 0,
            write ? NULL : buf, write ? 0 : len, &n);
    log_line("%s pipe=0x%x len=%lu timeout=%lu rc=%d n=%u ov=%p event=%p",
            write ? "Write" : "Read", pipe, len, timeout, rc, n, ov,
            ov ? ov->hEvent : NULL);
    if (ov) {
        ov->Internal = rc ? (rc == -7 ? 0xC00000B5u : 0xC0000001u) : 0;
        ov->InternalHigh = rc ? 0 : n;
        if (ov->hEvent)
            SetEvent(ov->hEvent);
    }
    if (rc) {
        SetLastError(map_err(rc));
        return FALSE;
    }
    if (transferred)
        *transferred = n;
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE iface,
        UCHAR pipe, PUCHAR buf, ULONG len, PULONG transferred, LPOVERLAPPED ov)
{
    struct dev *dev = dev_of(iface);
    if (!dev || !buf) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return transfer(dev, 0, pipe, buf, len, transferred, ov);
}

__declspec(dllexport) BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE iface,
        UCHAR pipe, PUCHAR buf, ULONG len, PULONG transferred, LPOVERLAPPED ov)
{
    struct dev *dev = dev_of(iface);
    if (!dev || (!buf && len)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return transfer(dev, 1, pipe, buf, len, transferred, ov);
}

__declspec(dllexport) BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE iface, UCHAR pipe)
{
    if (!dev_of(iface)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    int rc = call(CMD_ABORT, pipe, 0, 0, NULL, 0, NULL, 0, NULL);
    log_line("AbortPipe pipe=0x%x rc=%d", pipe, rc);
    if (rc) {
        SetLastError(map_err(rc));
        return FALSE;
    }
    return TRUE;
}
