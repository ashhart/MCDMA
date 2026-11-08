/* Shared parts of mcdma-rpcd: constants, the mailbox and the helpers every module uses. */
#ifndef MCDMA_RPCD_H
#define MCDMA_RPCD_H

#include "link.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PROTOCOL 1                      /* the mailbox protocol applications see */
#define RELEASE "2.0.0"
#if !defined(MCDMA_RPC_BOX_DIR) && !defined(__APPLE__)
#define MCDMA_RPC_BOX_DIR "/dev/shm"    /* Linux listen-end mailboxes; macOS uses POSIX shm; tests set a directory */
#endif
#ifndef MCDMA_RPC_LOCK_DIR
#define MCDMA_RPC_LOCK_DIR "/tmp"       /* one lock file per link name */
#endif
#ifndef MCDMA_RPC_LISTEN_LOCK
#define MCDMA_RPC_LISTEN_LOCK ""        /* tests that run both ends in one process give the listen end its own lock */
#endif
#ifndef MCDMA_RPC_SOCK_DIR
#define MCDMA_RPC_SOCK_DIR "/tmp"       /* Unix control sockets */
#endif
#ifndef MCDMA_RPC_HANDSHAKE_S
#define MCDMA_RPC_HANDSHAKE_S 10        /* an exchange must finish within this */
#endif
#ifndef MCDMA_RPC_LIVENESS_S
#define MCDMA_RPC_LIVENESS_S 6          /* a link whose peer has been silent this long is down */
#endif
#define IO_TIMEOUT_S 5                  /* bound on any single blocking socket call */
#define PENDING_S 5                     /* a socket client must send its command within this */
#define HANDSHAKE_NS ((uint64_t)MCDMA_RPC_HANDSHAKE_S * 1000000000ull)
#define LIVENESS_NS ((uint64_t)MCDMA_RPC_LIVENESS_S * 1000000000ull)
#define RESEND_NS 100000000ull          /* an unanswered offer is resent this often */
#define PING_NS (LIVENESS_NS / 6)       /* six pings fit in the liveness window */
#define XFER_NS 10000000000ull          /* one transfer must complete within this */
#define SEG LINK_SEG                    /* mailbox unit, and the RoCE registration unit on a Mac */
#define CTRL 4096ull
#define READ_CHUNK (2ull << 20)         /* the Mac provider's proven READ size, one outstanding at a time */
#define MAX_SEGS 64                     /* per half: 256 MiB */
#define MAX_PEERS 6
#define MAX_PENDING 4
#define LINE 16384
#define WINDOW_MAC 4
#define WINDOW_PEER 16
#define BULK (8u << 20)                 /* transfers at least this big are logged with their wire time */
#define WORD(seq, len) (((uint64_t)(uint32_t)(seq) << 32) | (uint32_t)(len))
#define WORD_SEQ(w) ((uint32_t)((w) >> 32))
#define WORD_LEN(w) ((uint32_t)(w))

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static inline uint64_t load_word(const volatile uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_word(volatile uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint64_t now_ns(void) { return link_now_ns(); }

/* The mailbox as a daemon sees it: request half [0, req), reply half [req, req + rep), registered as `r`. */
struct box {
    unsigned char *base;
    uint64_t req, rep;
    struct region r;
};

struct reader {
    int fd;
    char buf[LINE * 2];
    size_t len;
};

/* The user given the listen end's mailbox and socket when root runs the daemon for another user; set = 0 keeps the
   daemon's own user. */
struct owner {
    int set;
    uid_t uid;
    gid_t gid;
};

/* Set by SHUTDOWN, SIGINT or SIGTERM; every loop checks it and the normal teardown runs. */
extern volatile sig_atomic_t g_stop;

/* rpcd_common.c */
int hold_link_lock(const char *name, const char *suffix);
void set_io_timeout(int fd, int seconds);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_rate(const char *name, const char *what, uint64_t bytes, uint64_t ns);
int valid_name(const char *name);
int link_host(const char *name, char *out, size_t n);
void host_field(const char *name, char *out, size_t n);
int parse_mib(unsigned mib, uint64_t *out);
int send_line(int fd, const char *text);
int take_line(struct reader *r, char *out, size_t n, int block);
int socket_in_use(const char *path);
int unix_listen(const char *path);
const char *socket_path(const char *fallback);

/* rpcd_verbs.c */
void on_signal(int sig);
void write_sizes(struct box *b);
int box_register(struct ep *e, struct box *b, int listen);
void box_offer(const struct xchg *x, const struct ep *e, const struct box *b, int role, int mode, int flags,
               struct xmsg *m);
const char *box_refuse(const struct ep *e, const struct box *b, const struct xinfo *peer);
void xchg_answer(struct xchg *x, const uint8_t to[16], int kind, int role, const char *text);
int box_transfer(struct ep *e, struct box *b, const struct table *remote, uint64_t off, uint32_t len, uint64_t word_src,
                 uint64_t word_dst, uint64_t word, struct piece *pieces, int cap, int window);

/* rpcd_listen.c and rpcd_connect.c */
int run_listen(const char *name, const char *device, int gid_index, int mtu, const char *via, int port,
               uint64_t req_bytes, uint64_t rep_bytes, const struct owner *owner);
int run_connect(int npeers, char **specs, int direct);

#endif
