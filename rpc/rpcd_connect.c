/* mcdma-rpcd connect: one mailbox and one thread per peer link; each link's setup exchange runs over Thunderbolt IP. */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PIECES (4 * MAX_SEGS + 4)

struct peer {
    char name[32], via[96], device[64];
    int port, gid_index, mtu;
    struct ep e;
    struct box b;
    struct xchg x;
    struct table remote;           /* the listen end's mailbox, from its offer */
    struct xmsg confirm;           /* our confirming offer, sent again whenever the listen end offers again */
    int up;                        /* read by the control thread through __atomic loads */
    long long since;
    uint64_t calls, failures, bytes;
    uint64_t generation;           /* times the link has come up, published at request half +72 */
    uint64_t heard, pinged;
    struct piece pieces[PIECES];
    pthread_t thread;
};

static struct peer g_peers[MAX_PEERS];
static int g_npeers;
/* Direct replies need the Mac NIC to place a WRITE's payload before the next WRITE's word, i.e. relaxed ordering
   off (MCDMARelaxedOrdering = No); MCDMA_RPC_PULL=1 falls back to READing the payload, on RoCE only. */
static int g_direct = 1;

static void box_shm_name(const struct peer *p, char *out, size_t n) { snprintf(out, n, "/mcdma-rpc.%.*s", (int)sizeof(p->name), p->name); }

static int box_create(struct peer *p) {
    char name[64];
    box_shm_name(p, name, sizeof(name));
    shm_unlink(name);
    uint64_t total = p->b.req + p->b.rep;
    int f = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (f < 0 || ftruncate(f, (off_t)total)) {
        logf_("shm %s errno=%d", name, errno);
        if (f >= 0) close(f);
        return -1;
    }
    p->b.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    if (p->b.base == MAP_FAILED) {
        p->b.base = NULL;
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    for (uint64_t off = 0; off < total; off += (uint64_t)page) p->b.base[off] = 0;
    if (box_register(&p->e, &p->b, 0)) return -1;
    write_sizes(&p->b);
    return 0;
}

static int mode(void) { return g_direct ? MODE_DIRECT : MODE_PULL; }

static int session_known(const struct xchg *x) {
    static const uint8_t zero[16];
    return memcmp(x->peer_session, zero, 16) != 0;
}

static void peer_down(struct peer *p, const char *why) {
    if (__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) logf_("%s: down (%s)", p->name, why);
    __atomic_store_n(&p->up, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->since, 0, __ATOMIC_RELAXED);
    if (p->b.base) store_word((volatile uint64_t *)(p->b.base + 64), 0);
    /* like closing a connection: the listen end drops the link at once instead of waiting out its liveness */
    if (p->x.fd >= 0 && session_known(&p->x)) xchg_answer(&p->x, p->x.peer_session, X_BYE, ROLE_CONNECT, NULL);
    memset(p->x.peer_session, 0, 16);
    ep_destroy_qp(&p->e);                   /* waits out anything still posted first */
}

/* Bring the link up; `last` receives the request word as it stood before clients could see the link up. */
static int peer_connect(struct peer *p, uint32_t *last) {
    if (p->x.fd < 0 && xchg_open(&p->x, p->via, 0, p->port, p->name)) return -1;
    if (ep_create_qp(&p->e)) {
        peer_down(p, "qp");
        return -1;
    }
    if (p->e.kind == LINK_TB) tb_accept(&p->e, &p->b.r, p->b.req, p->b.req + p->b.rep);
    xchg_session(&p->x);
    /* each attempt finds the listen end by multicast on the cable, in case its address changed */
    if (!p->x.pinned) p->x.learned = 0;
    struct xmsg offer, got;
    box_offer(&p->x, &p->e, &p->b, ROLE_CONNECT, mode(), 0, &offer);
    /* every step is bounded, so SHUTDOWN is never stuck behind a listen end that stays silent */
    uint64_t deadline = now_ns() + HANDSHAKE_NS, resend = 0;
    const char *why = "no answer";
    int answered = 0;
    while (!g_stop && !answered && now_ns() < deadline) {
        if (now_ns() >= resend) {
            (void)xchg_send(&p->x, &offer);   /* fails while the cable or interface is down; the attempt goes on */
            resend = now_ns() + RESEND_NS;
        }
        int r = xchg_recv(&p->x, &got, 20);
        if (r < 0) {
            why = "exchange socket";
            break;
        }
        if (r == 0 || got.role != ROLE_LISTEN || !xchg_ours(&p->x, &got)) continue;
        if (got.kind == X_ERR || got.kind == X_BYE) {
            /* the listen end refused: our queue pair never left INIT and nothing was posted */
            logf_("%s: handshake refused (%s)", p->name, got.kind == X_ERR ? got.text : "goodbye");
            why = "handshake";
            break;
        }
        if (got.kind != X_OFFER || !(got.flags & X_HAVE)) continue;
        if ((why = box_refuse(&p->e, &p->b, &got.info))) {
            logf_("%s: cannot use the listen end's offer: %s", p->name, why);
            xchg_answer(&p->x, got.from, X_ERR, ROLE_CONNECT, why);
            break;
        }
        answered = 1;
    }
    if (!answered) {
        peer_down(p, g_stop ? "shutdown" : why);
        return -1;
    }
    memcpy(p->x.peer_session, got.from, 16);
    if (ep_connect(&p->e, &got.info)) {
        peer_down(p, "rtr/rts");
        return -1;
    }
    p->remote = got.info.table;
    /* our side is at RTS now: confirm, and the listen end arms; it offers again until it hears this */
    box_offer(&p->x, &p->e, &p->b, ROLE_CONNECT, mode(), X_HAVE, &p->confirm);
    (void)xchg_send(&p->x, &p->confirm);
    store_word((volatile uint64_t *)p->b.base, 0);
    store_word((volatile uint64_t *)(p->b.base + p->b.req), 0);
    store_word((volatile uint64_t *)(p->b.base + p->b.req + 64), 0);
    /* taken before clients can see the link up, so a request staged the moment they do is still sent */
    *last = WORD_SEQ(load_word((volatile uint64_t *)p->b.base));
    __atomic_store_n(&p->since, (long long)time(NULL), __ATOMIC_RELAXED);
    __atomic_store_n(&p->up, 1, __ATOMIC_RELEASE);
    /* a new generation tells a waiting client that a request staged before the reconnect was lost */
    store_word((volatile uint64_t *)(p->b.base + 72), ++p->generation);
    store_word((volatile uint64_t *)(p->b.base + 64), 1);
    p->heard = p->pinged = now_ns();
    logf_("%s: connected over %s (%s qpn %u <-> %u, mailbox %" PRIu64 " + %" PRIu64 " MiB, %s replies, exchange on %s)",
          p->name, link_kind_name(p->e.kind), p->device, p->e.qp->qp_num, got.info.qpn, p->b.req >> 20,
          p->b.rep >> 20, g_direct ? "direct" : "pull", p->x.ifname);
    return 0;
}

/* Datagrams while the link is up: answers to our pings, a listen end that has not heard our confirmation, goodbyes. */
static void connect_service(struct peer *p) {
    struct xmsg m;
    uint64_t now = now_ns();
    while (__atomic_load_n(&p->up, __ATOMIC_ACQUIRE) && xchg_recv(&p->x, &m, 0) == 1) {
        if (m.role != ROLE_LISTEN || !xchg_ours(&p->x, &m)) continue;
        if (m.kind == X_BYE || m.kind == X_ERR) {
            peer_down(p, m.kind == X_ERR ? m.text : "the listen end said goodbye");
            return;
        }
        if (memcmp(m.from, p->x.peer_session, 16)) continue;
        p->heard = now;
        if (m.kind == X_OFFER) (void)xchg_send(&p->x, &p->confirm);
    }
    if (!__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) return;
    if (now - p->pinged >= PING_NS) {
        xchg_answer(&p->x, p->x.peer_session, X_PING, ROLE_CONNECT, NULL);
        p->pinged = now;
    }
    if (now - p->heard > LIVENESS_NS) peer_down(p, "the listen end is silent");
}

static void *peer_thread(void *arg) {
    struct peer *p = arg;
    volatile uint64_t *req = (volatile uint64_t *)p->b.base;
    volatile uint64_t *ready = (volatile uint64_t *)(p->b.base + p->b.req);
    volatile uint64_t *done = (volatile uint64_t *)(p->b.base + p->b.req + 64);
    uint32_t last = 0;
    uint64_t active = now_ns();
    unsigned idle = 0;
    while (!g_stop) {
        if (!__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) {
            if (peer_connect(p, &last)) {
                for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000);
                continue;
            }
        }
        /* Thunderbolt replies land only when this end posts receives for them */
        if (p->e.kind == LINK_TB) {
            int handled = tb_progress(&p->e);
            if (handled < 0) {
                __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
                peer_down(p, "thunderbolt link failed");
                continue;
            }
            if (handled) active = now_ns();
        }
        uint64_t w = load_word(req);
        uint32_t seq = WORD_SEQ(w), len = WORD_LEN(w);
        if (!seq || seq == last) {
            if (now_ns() - active > 50000000ull)
                usleep(20);
            else
                cpu_relax();
            if (++idle >= 4096) {
                idle = 0;
                connect_service(p);
            }
            continue;
        }
        last = seq;
        active = now_ns();
        if (len > p->b.req - CTRL) {
            store_word(done, WORD(seq, 0));
            continue;
        }
        /* payload first, then the word: RC places the WRITEs in order, and Thunderbolt applies the word after them */
        uint64_t began = now_ns();
        if (box_transfer(&p->e, &p->b, &p->remote, CTRL, len, 0, 0, w, p->pieces, PIECES, WINDOW_MAC)) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "request write");
            continue;
        }
        /* a Thunderbolt write returns once posted, so only RoCE times the wire */
        if (len >= BULK && p->e.kind == LINK_ROCE) log_rate(p->name, "request", len, now_ns() - began);
        __atomic_fetch_add(&p->bytes, len, __ATOMIC_RELAXED);
        if (g_direct) {   /* the peer writes the reply and the client's word itself */
            __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint64_t start = now_ns(), r = 0;
        unsigned spins = 0;
        while (!g_stop && WORD_SEQ(r = load_word(ready)) != seq) {
            cpu_relax();
            if (++spins == 4096) {
                spins = 0;
                if (now_ns() - start > 30000000000ull) break;
                connect_service(p);
                if (!__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) break;
            }
        }
        if (!__atomic_load_n(&p->up, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (WORD_SEQ(r) != seq) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, g_stop ? "shutdown during a call" : "no reply in 30 s");
            continue;
        }
        uint32_t rlen = WORD_LEN(r);
        /* the length comes from the peer: never let it reach past the reply half */
        if (rlen > p->b.rep - CTRL) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "reply longer than the reply half");
            continue;
        }
        /* READ the reply one piece at a time (the Mac provider allows one outstanding READ per QP) */
        int n = cut(&p->b.r, p->b.req + CTRL, rlen, &p->remote, p->b.req + CTRL, READ_CHUNK, p->pieces, PIECES);
        if (n < 0 || post_pieces(&p->e, IBV_WR_RDMA_READ, p->pieces, n, 1, XFER_NS)) {
            __atomic_fetch_add(&p->failures, 1, __ATOMIC_RELAXED);
            peer_down(p, "reply read");
            continue;
        }
        store_word(done, WORD(seq, rlen));
        __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static int parse_peer(const char *spec, struct peer *p) {
    memset(p, 0, sizeof(*p));
    p->x.fd = -1;
    unsigned req_mib = 4, rep_mib = 4;
    char ifname[32];
    struct in6_addr pin;
    int pinned = 0;
    int n = sscanf(spec, "%31[^,],%95[^,],%d,%63[^,],%d,%d,%u,%u", p->name, p->via, &p->port, p->device,
                   &p->gid_index, &p->mtu, &req_mib, &rep_mib);
    if ((n != 6 && n != 8) || !valid_name(p->name) || p->port <= 0 || p->port > 65535 ||
        via_parse(p->via, ifname, sizeof(ifname), &pin, &pinned))
        return -1;
    return parse_mib(req_mib, &p->b.req) || parse_mib(rep_mib, &p->b.rep) ? -1 : 0;
}

static void connect_status(int fd) {
    char out[512];
    snprintf(out, sizeof(out), "VERSION mcdma-rpcd %d %s", PROTOCOL, RELEASE);
    send_line(fd, out);
    for (int i = 0; i < g_npeers; ++i) {
        struct peer *p = &g_peers[i];
        int up = __atomic_load_n(&p->up, __ATOMIC_ACQUIRE);
        snprintf(out, sizeof(out),
                 "PEER %.*s %s calls %" PRIu64 " failures %" PRIu64 " MiB %" PRIu64
                 " via=%.*s port=%d device=%.*s link=%s req_mib=%" PRIu64 " rep_mib=%" PRIu64 " since=%lld",
                 (int)sizeof(p->name), p->name, up ? "up" : "down", __atomic_load_n(&p->calls, __ATOMIC_RELAXED),
                 __atomic_load_n(&p->failures, __ATOMIC_RELAXED), __atomic_load_n(&p->bytes, __ATOMIC_RELAXED) >> 20,
                 (int)sizeof(p->via), p->via, p->port, (int)sizeof(p->device), p->device,
                 link_kind_name(p->e.kind), p->b.req >> 20, p->b.rep >> 20,
                 up ? __atomic_load_n(&p->since, __ATOMIC_RELAXED) : 0);
        send_line(fd, out);
    }
    send_line(fd, "END");
}

/* Close the endpoints and remove the mailboxes of the first `count` peers, after a failed start or at exit. */
static void release_peers(int count) {
    for (int i = 0; i < count; ++i) {
        char name[64];
        xchg_close(&g_peers[i].x);
        ep_close(&g_peers[i].e);
        box_shm_name(&g_peers[i], name, sizeof(name));
        shm_unlink(name);
    }
}

int run_connect(int npeers, char **specs, int direct) {
    if (npeers > MAX_PEERS) {
        logf_("at most %d peers", MAX_PEERS);
        return 2;
    }
    g_direct = direct;
    for (int i = 0; i < npeers; ++i) {
        if (parse_peer(specs[i], &g_peers[i])) {
            logf_("bad peer %s", specs[i]);
            return 2;
        }
        for (int j = 0; j < i; ++j)
            if (!strcmp(g_peers[j].name, g_peers[i].name)) {
                logf_("peer name %s is used twice", g_peers[i].name);
                return 2;
            }
    }
    /* locks and the socket come before any device or mailbox, so a running daemon is never orphaned */
    for (int i = 0; i < npeers; ++i)
        if (hold_link_lock(g_peers[i].name, "") < 0) return 2;
    const char *sock_path = socket_path(MCDMA_RPC_SOCK_DIR "/mcdma-rpcd.sock");
    if (socket_in_use(sock_path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", sock_path);
        return 2;
    }
    int us = unix_listen(sock_path);
    if (us < 0) return 2;
    for (int i = 0; i < npeers; ++i) {
        struct peer *p = &g_peers[i];
        g_npeers = i + 1;
        int bad = ep_open(&p->e, p->device, p->gid_index, p->mtu);
        if (!bad && !g_direct && p->e.kind == LINK_TB) {
            logf_("%s: MCDMA_RPC_PULL=1 needs RDMA READ, which Thunderbolt lacks", p->name);
            bad = 1;
        }
        bad = bad || via_check(p->via, &p->e);
        if (bad || box_create(p)) {
            release_peers(g_npeers);
            close(us);
            unlink(sock_path);
            return 2;
        }
    }
    int started = 0;
    while (started < g_npeers && !pthread_create(&g_peers[started].thread, NULL, peer_thread, &g_peers[started]))
        started++;
    if (started < g_npeers) {
        logf_("could not start the thread for %s", g_peers[started].name);
        g_stop = 1;
    } else {
        logf_("connect: %d peers, control socket %s", g_npeers, sock_path);
    }
    char *line = malloc(LINE);
    static struct reader rd;
    while (!g_stop && line) {
        /* a short tick, so a signal that lands on any thread still ends the loop promptly */
        struct pollfd wait = {.fd = us, .events = POLLIN};
        if (poll(&wait, 1, 500) <= 0) continue;
        int fd = accept(us, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        /* a silent client is dropped after a few seconds, so it can never hold SHUTDOWN off */
        set_io_timeout(fd, IO_TIMEOUT_S);
        memset(&rd, 0, sizeof(rd));
        rd.fd = fd;
        while (take_line(&rd, line, LINE, 1) == 1) {
            if (!strcmp(line, "STATUS")) {
                connect_status(fd);
            } else if (!strcmp(line, "SHUTDOWN")) {
                g_stop = 1;
                send_line(fd, "BYE");
                break;
            } else {
                send_line(fd, "ERR unknown command");
            }
        }
        close(fd);
    }
    g_stop = 1;
    for (int i = 0; i < started; ++i) pthread_join(g_peers[i].thread, NULL);
    for (int i = 0; i < g_npeers; ++i) peer_down(&g_peers[i], "shutdown");
    release_peers(g_npeers);
    free(line);
    close(us);
    unlink(sock_path);
    logf_("connect: every verbs object destroyed, exiting");
    return started < g_npeers ? 2 : 0;
}
