/* TCP stand-in for the NI agent sockets. Ableton's Wine has no AF_UNIX,
 * so a hook in that prefix connects here and this process attaches the
 * real socket in the NI prefix. */
#include <errno.h>
#include <stddef.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>

#define PORT 47654

static FILE *logf;
static char agent_root[400];

static int set_agent_root(void)
{
    const char *env = getenv("NI_AGENT_DIR");
    size_t n;
    if (!env || !env[0]) {
        fprintf(stderr, "set NI_AGENT_DIR to the NI Public Documents directory\n");
        return -1;
    }
    n = strlen(env);
    if (n + 2 >= sizeof agent_root)
        return -1;
    memcpy(agent_root, env, n + 1);
    if (agent_root[n - 1] != '/') {
        agent_root[n] = '/';
        agent_root[n + 1] = 0;
    }
    return 0;
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

static int open_agent(const char *path)
{
    const char *name = NULL;
    char full[512];
    if (strstr(path, "odr_agent"))
        name = "com.native-instruments.odr_agent.kks";
    else if (strstr(path, "nks-connect"))
        name = "com.native-instruments.nks-connect";
    if (!name)
        return -1;
    snprintf(full, sizeof full, "%s%s", agent_root, name);
    if (strlen(full) >= sizeof(struct sockaddr_un) - offsetof(struct sockaddr_un, sun_path)) {
        log_msg("socket path too long: %s", full);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", full);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pump(int a, int b)
{
    char buf[16384];
    for (;;) {
        struct pollfd pfd[2] = {
            { a, POLLIN, 0 },
            { b, POLLIN, 0 },
        };
        int rc = poll(pfd, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        for (int i = 0; i < 2; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int src = pfd[i].fd;
            int dst = src == a ? b : a;
            ssize_t n = recv(src, buf, sizeof buf, 0);
            if (n <= 0)
                return;
            if (write_full(dst, buf, (size_t)n))
                return;
        }
    }
}

static void handle(int tcp)
{
    uint32_t n = 0;
    if (read_full(tcp, &n, sizeof n) || n == 0 || n > 400)
        return;
    char path[401];
    if (read_full(tcp, path, n))
        return;
    path[n] = 0;
    int unix_fd = open_agent(path);
    unsigned char ack = unix_fd >= 0 ? 0 : 1;
    log_msg("client path=%s unix=%d", path, unix_fd);
    if (write_full(tcp, &ack, 1) || ack)
        return;
    pump(tcp, unix_fd);
    close(unix_fd);
}

int main(void)
{
    if (set_agent_root())
        return 1;
    logf = fopen("/tmp/ni-nks-bridge.log", "a");
    if (!logf)
        return 1;
    setvbuf(logf, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) || listen(srv, 16)) {
        log_msg("listen failed: %s", strerror(errno));
        return 1;
    }
    log_msg("listening on 127.0.0.1:%d", PORT);
    for (;;) {
        int tcp = accept(srv, NULL, NULL);
        if (tcp < 0)
            continue;
        setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (fork() == 0) {
            close(srv);
            handle(tcp);
            close(tcp);
            _exit(0);
        }
        close(tcp);
    }
}
