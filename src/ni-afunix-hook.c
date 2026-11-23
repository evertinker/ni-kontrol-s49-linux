/* Loaded into the Ableton prefix only. socket(AF_UNIX) becomes a TCP
 * socket to the local bridge, which owns the real NI agent socket. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BRIDGE_PORT 47654
#define MAX_TRACK 64

typedef SOCKET (WSAAPI *socket_fn)(int, int, int);
typedef SOCKET (WSAAPI *wsasocketw_fn)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
typedef SOCKET (WSAAPI *wsasocketa_fn)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
typedef int (WSAAPI *connect_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *wsaconnect_fn)(SOCKET, const struct sockaddr *, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef int (WSAAPI *send_fn)(SOCKET, const char *, int, int);
typedef int (WSAAPI *recv_fn)(SOCKET, char *, int, int);
typedef int (WSAAPI *ioctlsocket_fn)(SOCKET, long, u_long *);
typedef int (WSAAPI *closesocket_fn)(SOCKET);

static socket_fn real_socket;
static wsasocketw_fn real_wsasocketw;
static wsasocketa_fn real_wsasocketa;
static connect_fn real_connect;
static wsaconnect_fn real_wsaconnect;
static send_fn real_send;
static recv_fn real_recv;
static ioctlsocket_fn real_ioctl;
static closesocket_fn real_close;

static CRITICAL_SECTION mu;
static SOCKET tracked[MAX_TRACK];
static int ready;

static void log_line(const char *fmt, ...)
{
    FILE *f = fopen("Z:\\tmp\\ni-afunix-hook.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int (WSAAPI *real_getsockopt)(SOCKET, int, int, char *, int *);

static int socket_dead(SOCKET s)
{
    int type = 0;
    int len = sizeof type;
    if (!real_getsockopt)
        return 0;
    return real_getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0;
}

static void track_add(SOCKET s)
{
    EnterCriticalSection(&mu);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_TRACK; i++) {
            if (!tracked[i] || (pass == 1 && socket_dead(tracked[i]))) {
                tracked[i] = s;
                LeaveCriticalSection(&mu);
                return;
            }
        }
    }
    LeaveCriticalSection(&mu);
    log_line("track table full");
}

static int track_has(SOCKET s)
{
    int yes = 0;
    EnterCriticalSection(&mu);
    for (int i = 0; i < MAX_TRACK; i++) {
        if (tracked[i] == s) {
            yes = 1;
            break;
        }
    }
    LeaveCriticalSection(&mu);
    return yes;
}

static void track_del(SOCKET s)
{
    EnterCriticalSection(&mu);
    for (int i = 0; i < MAX_TRACK; i++) {
        if (tracked[i] == s)
            tracked[i] = 0;
    }
    LeaveCriticalSection(&mu);
}

static void *tramp(void *target, const unsigned char *expect, int n, void *hook)
{
    unsigned char *code = target;
    if (memcmp(code, expect, (size_t)n) != 0 || n < 14) {
        log_line("prologue mismatch at %p", target);
        return NULL;
    }
    unsigned char *gate = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!gate)
        return NULL;
    memcpy(gate, code, (size_t)n);
    gate[n] = 0xFF;
    gate[n + 1] = 0x25;
    *(uint32_t *)(gate + n + 2) = 0;
    *(uint64_t *)(gate + n + 6) = (uint64_t)(code + n);
    DWORD old = 0;
    if (!VirtualProtect(code, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    unsigned char jmp[32];
    memset(jmp, 0x90, sizeof jmp);
    jmp[0] = 0xFF;
    jmp[1] = 0x25;
    *(uint32_t *)(jmp + 2) = 0;
    *(uint64_t *)(jmp + 6) = (uint64_t)hook;
    memcpy(code, jmp, (size_t)n);
    VirtualProtect(code, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, (SIZE_T)n);
    return gate;
}

static SOCKET tcp_socket(DWORD flags)
{
    SOCKET s;
    if (flags == 0)
        s = real_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    else
        s = real_wsasocketw(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, flags);
    if (s != INVALID_SOCKET)
        track_add(s);
    return s;
}

static SOCKET WINAPI hook_socket(int af, int type, int protocol)
{
    if (af == AF_UNIX && type == SOCK_STREAM)
        return tcp_socket(0);
    return real_socket(af, type, protocol);
}

static SOCKET WINAPI hook_wsasocketw(int af, int type, int protocol,
        LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags)
{
    (void)info;
    (void)g;
    if (af == AF_UNIX && type == SOCK_STREAM)
        return tcp_socket(flags);
    return real_wsasocketw(af, type, protocol, info, g, flags);
}

static SOCKET WINAPI hook_wsasocketa(int af, int type, int protocol,
        LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags)
{
    (void)info;
    (void)g;
    if (af == AF_UNIX && type == SOCK_STREAM)
        return tcp_socket(flags);
    return real_wsasocketa(af, type, protocol, info, g, flags);
}

static int bridge_connect(SOCKET s, const char *path)
{
    struct sockaddr_in in;
    u_long nb = 0;
    unsigned char ack = 1;
    uint32_t n;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(BRIDGE_PORT);
    /* Finish the handshake before returning, even if the caller
     * asked for a nonblocking socket. */
    nb = 0;
    real_ioctl(s, FIONBIO, &nb);
    if (real_connect(s, (struct sockaddr *)&in, sizeof in)) {
        int err = WSAGetLastError();
        log_line("bridge connect failed %d", err);
        WSASetLastError(err);
        return SOCKET_ERROR;
    }
    n = (uint32_t)strlen(path);
    if (real_send(s, (const char *)&n, sizeof n, 0) != (int)sizeof n
            || real_send(s, path, (int)n, 0) != (int)n
            || real_recv(s, (char *)&ack, 1, MSG_WAITALL) != 1
            || ack) {
        log_line("bridge handshake failed path=%s ack=%u", path, ack);
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    log_line("bridged %s", path);
    return 0;
}

static int unix_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    char path[256];
    int n;
    if (!track_has(s) || !name || name->sa_family != AF_UNIX || namelen < 4)
        return 1;
    n = namelen - 2;
    if (n > (int)sizeof path - 1)
        n = (int)sizeof path - 1;
    memcpy(path, name->sa_data, (size_t)n);
    path[n] = 0;
    return bridge_connect(s, path);
}

static int WINAPI hook_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    int rc = unix_connect(s, name, namelen);
    if (rc != 1)
        return rc;
    return real_connect(s, name, namelen);
}

static int WINAPI hook_wsaconnect(SOCKET s, const struct sockaddr *name, int namelen,
        LPWSABUF caller, LPWSABUF callee, LPQOS sqos, LPQOS gqos)
{
    int rc = unix_connect(s, name, namelen);
    if (rc != 1)
        return rc;
    return real_wsaconnect(s, name, namelen, caller, callee, sqos, gqos);
}

static int WINAPI hook_close(SOCKET s)
{
    track_del(s);
    return real_close(s);
}

static int install(void)
{
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    static const unsigned char socket_b[] = {
        0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x44, 0x89, 0xC6
    };
    static const unsigned char w_b[] = {
        0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53,
        0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00
    };
    static const unsigned char a_b[] = {
        0x41, 0x56, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x02, 0x00, 0x00, 0x4D, 0x85, 0xC9
    };
    static const unsigned char c_b[] = {
        0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x68, 0x44, 0x89, 0xC3
    };
    static const unsigned char wc_b[] = {
        0x66, 0x90, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x49, 0x0F, 0x6E, 0xC1
    };
    static const unsigned char cl_b[] = {
        0x66, 0x90, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xCE
    };
    if (!ws)
        return 0;
    real_ioctl = (ioctlsocket_fn)GetProcAddress(ws, "ioctlsocket");
    real_getsockopt = (int (WSAAPI *)(SOCKET, int, int, char *, int *))GetProcAddress(ws, "getsockopt");
    real_send = (send_fn)GetProcAddress(ws, "send");
    real_recv = (recv_fn)GetProcAddress(ws, "recv");
    real_socket = tramp(GetProcAddress(ws, "socket"), socket_b, sizeof socket_b, hook_socket);
    real_wsasocketw = tramp(GetProcAddress(ws, "WSASocketW"), w_b, sizeof w_b, hook_wsasocketw);
    real_wsasocketa = tramp(GetProcAddress(ws, "WSASocketA"), a_b, sizeof a_b, hook_wsasocketa);
    real_connect = tramp(GetProcAddress(ws, "connect"), c_b, sizeof c_b, hook_connect);
    real_wsaconnect = tramp(GetProcAddress(ws, "WSAConnect"), wc_b, sizeof wc_b, hook_wsaconnect);
    /* closesocket's next instruction is RIP-relative, so only a short
     * position-independent prefix can be moved. Skip the hook if it does
     * not fit; the track table is small and Ableton opens few sockets. */
    if (sizeof cl_b >= 14)
        real_close = tramp(GetProcAddress(ws, "closesocket"), cl_b, sizeof cl_b, hook_close);
    if (!real_socket || !real_wsasocketw || !real_connect || !real_send || !real_recv || !real_ioctl) {
        log_line("hook incomplete");
        return 0;
    }
    log_line("hooks installed");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&mu);
        ready = install();
    }
    return TRUE;
}
