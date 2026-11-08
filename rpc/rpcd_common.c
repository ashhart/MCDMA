/* mcdma-rpcd: logging, names, locks and line I/O on the local control and service sockets. */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop;

static void vlog(const char *fmt, va_list ap) {
    fprintf(stderr, "mcdma-rpcd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

/* The shared link code logs through the daemon's own log. */
void link_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void log_rate(const char *name, const char *what, uint64_t bytes, uint64_t ns) {
    logf_("%s: %s %.1f MiB in %.2f ms (%.1f Gbit/s)", name, what, (double)bytes / 1048576.0, (double)ns / 1e6,
          ns ? (double)bytes * 8.0 / (double)ns : 0.0);
}

/* Hold an exclusive lock on MCDMA_RPC_LOCK_DIR/mcdma-rpc.NAME.lock until exit, so one daemon serves each link. */
int hold_link_lock(const char *name, const char *suffix) {
    char path[192];
    snprintf(path, sizeof(path), "%s/mcdma-rpc.%s%s.lock", MCDMA_RPC_LOCK_DIR, name, suffix);
    /* open before create: in a sticky directory, O_CREAT over another user's file is refused */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = open(path, O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) fchmod(fd, 0644);
        else if (errno == EEXIST) fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        logf_("lock %s errno=%d", path, errno);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        logf_("another mcdma-rpcd serves link %s; stop it with SHUTDOWN first", name);
        close(fd);
        return -1;
    }
    return fd;
}

/* Bound every blocking send and receive on `fd`, so no silent peer can hold a thread forever. */
void set_io_timeout(int fd, int seconds) {
    struct timeval limit = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

int valid_name(const char *name) {
    size_t n = strlen(name);
    if (n < 1 || n > 20) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* The HOST that MCDMA_RPCD_HOSTS="NAME=HOST[,NAME=HOST]" gives link `name`: the name an application knows the peer
 * by, which STATUS reports as host= (oMLX ties a link to an enrolled worker through it). The exchange never needs
 * one. 1 and the host in `out`, 0 when the link has no label, -1 when the variable is malformed: an entry without
 * NAME=HOST, a NAME that is not a link name, an empty HOST or one over 63 characters, or a HOST with whitespace. */
int link_host(const char *name, char *out, size_t n) {
    const char *env = getenv("MCDMA_RPCD_HOSTS");
    int found = 0;
    if (out && n) *out = 0;
    for (const char *at = env; at && *at;) {
        const char *end = strchr(at, ',');
        size_t entry = end ? (size_t)(end - at) : strlen(at);
        const char *eq = memchr(at, '=', entry);
        char label[21];
        if (!eq || (size_t)(eq - at) >= sizeof(label)) return -1;
        memcpy(label, at, (size_t)(eq - at));
        label[eq - at] = 0;
        const char *host = eq + 1;
        size_t len = entry - (size_t)(eq - at) - 1;
        if (!valid_name(label) || !len || len > 63) return -1;
        for (size_t i = 0; i < len; ++i)
            if ((unsigned char)host[i] <= ' ' || host[i] == 127) return -1;
        if (!found && !strcmp(label, name)) {
            if (out && n) snprintf(out, n, "%.*s", (int)len, host);
            found = 1;
        }
        if (end && !end[1]) return -1;   /* a trailing comma */
        at = end ? end + 1 : at + entry;
    }
    return found;
}

/* " host=HOST" for a STATUS line, or "" for a link with no label. */
void host_field(const char *name, char *out, size_t n) {
    char host[64];
    if (n) *out = 0;
    if (link_host(name, host, sizeof(host)) > 0) snprintf(out, n, " host=%s", host);
}

int parse_mib(unsigned mib, uint64_t *out) {
    if (!mib || mib % 4 || mib > 4 * MAX_SEGS) return -1;
    *out = (uint64_t)mib << 20;
    return 0;
}

int send_line(int fd, const char *text) {
    size_t n = strlen(text);
    char *buf = malloc(n + 1);
    if (!buf) return -1;
    memcpy(buf, text, n);
    buf[n] = '\n';
    int rc = 0;
    for (size_t off = 0; off < n + 1;) {
        ssize_t w = send(fd, buf + off, n + 1 - off, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }
    free(buf);
    return rc;
}

/* One line; 1 if a line is ready, 0 if none yet (non-blocking use), -1 on EOF or error. */
int take_line(struct reader *r, char *out, size_t n, int block) {
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl) {
            size_t line = (size_t)(nl - r->buf);
            if (line >= n) return -1;
            memcpy(out, r->buf, line);
            out[line] = 0;
            memmove(r->buf, nl + 1, r->len - line - 1);
            r->len -= line + 1;
            return 1;
        }
        if (r->len == sizeof(r->buf)) return -1;
        ssize_t got = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, block ? 0 : MSG_DONTWAIT);
        if (got == 0) return -1;
        if (got < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;
                continue;
            }
            if (!block && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            return -1;
        }
        r->len += (size_t)got;
    }
}

/* 1 when a daemon is accepting on `path`; a socket file left by a stopped daemon refuses and reads as free. */
int socket_in_use(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) return 0;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int live = fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (fd >= 0) close(fd);
    return live;
}

int unix_listen(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        logf_("unix socket path too long: %s", path);
        return -1;
    }
    /* never take a live daemon's socket: it could then be stopped only by a signal, with its QPs still open */
    if (socket_in_use(path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", path);
        return -1;
    }
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(path);
    mode_t old = umask(077);
    int bound = fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof(a));
    umask(old);
    if (!bound || listen(fd, 4) || chmod(path, 0600)) {
        logf_("unix socket %s errno=%d", path, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

const char *socket_path(const char *fallback) {
    const char *env = getenv("MCDMA_RPCD_SOCKET");
    return env && *env ? env : fallback;
}
