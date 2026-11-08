/* mcdma-rpcd listen: one end of one link, on Linux or a Mac, serving one registered service through its mailbox. */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PENDING_NS ((uint64_t)PENDING_S * 1000000000ull)
#define PIECES (4 * MAX_SEGS + 4)

struct listen_state {
    const char *name;
    struct ep e;
    struct box b;
    struct xchg x;
    struct table remote;           /* the connect end's mailbox, from its offer */
    struct xmsg answer;            /* our offer for the current session, sent again until it is confirmed */
    int svc, direct;
    int hello;                     /* our queue pairs are at RTS for the current session */
    int armed;                     /* the connect end confirmed its RTS: replies may be written */
    uint64_t hello_since, offered, heard;
    uint64_t served, failures, bytes;
    long long since;
    struct piece pieces[PIECES];
};

static void listen_status(struct listen_state *s, int fd) {
    char out[512], host[80];
    snprintf(out, sizeof(out), "VERSION mcdma-rpcd %d %s", PROTOCOL, RELEASE);
    send_line(fd, out);
    host_field(s->name, host, sizeof(host));
    snprintf(out, sizeof(out),
             "PEER %s %s calls %" PRIu64 " failures %" PRIu64 " MiB %" PRIu64
             " device=%s link=%s%s via=%s port=%d req_mib=%" PRIu64 " rep_mib=%" PRIu64 " since=%lld service=%s",
             s->name, s->armed ? "up" : "down", s->served, s->failures, s->bytes >> 20, s->e.device,
             link_kind_name(s->e.kind), host, s->x.ifname, s->x.port, s->b.req >> 20, s->b.rep >> 20,
             s->armed ? s->since : 0, s->svc >= 0 ? "attached" : "none");
    send_line(fd, out);
    send_line(fd, "END");
}

static void listen_drop(struct listen_state *s, const char *why) {
    static const uint8_t zero[16];
    if (s->armed) logf_("%s: link down (%s)", s->name, why);
    /* the service's requests died with the link: ending its registration lets it fail fast instead of waiting */
    if (s->svc >= 0 && (s->armed || s->hello)) {
        send_line(s->svc, "BYE");
        close(s->svc);
        s->svc = -1;
    }
    if (memcmp(s->x.peer_session, zero, 16)) xchg_answer(&s->x, s->x.peer_session, X_BYE, ROLE_LISTEN, NULL);
    memset(s->x.peer_session, 0, 16);
    s->armed = 0;
    s->hello = 0;
    ep_destroy_qp(&s->e);
}

/* A new session: our queue pairs reach RTS before we answer, so a failure never leaves the peer holding a live one. */
static void listen_offer(struct listen_state *s, const struct xmsg *m) {
    volatile uint64_t *req_word = (volatile uint64_t *)s->b.base;
    volatile uint64_t *staged_word = (volatile uint64_t *)(s->b.base + s->b.req + 128);
    if (s->hello) listen_drop(s, "the connect end started a new session");
    const char *why = box_refuse(&s->e, &s->b, &m->info);
    if (why) {
        logf_("%s: refusing an offer: %s", s->name, why);
        xchg_answer(&s->x, m->from, X_ERR, ROLE_LISTEN, why);
        return;
    }
    if (ep_create_qp(&s->e)) {
        xchg_answer(&s->x, m->from, X_ERR, ROLE_LISTEN, "qp");
        return;
    }
    if (s->e.kind == LINK_TB) tb_accept(&s->e, &s->b.r, 0, s->b.req);
    if (ep_connect(&s->e, &m->info)) {
        xchg_answer(&s->x, m->from, X_ERR, ROLE_LISTEN, "qp");
        ep_destroy_qp(&s->e);
        return;
    }
    xchg_session(&s->x);
    memcpy(s->x.peer_session, m->from, 16);
    s->remote = m->info.table;
    s->direct = m->info.mode == MODE_DIRECT;
    store_word(req_word, 0);
    store_word(staged_word, 0);
    box_offer(&s->x, &s->e, &s->b, ROLE_LISTEN, m->info.mode, X_HAVE, &s->answer);
    (void)xchg_send(&s->x, &s->answer);
    s->hello = 1;
    s->hello_since = s->offered = s->heard = now_ns();
}

/* 1 when the datagram began a new session, so the caller forgets the last staged reply. */
static int listen_datagram(struct listen_state *s, const struct xmsg *m) {
    if (m->role != ROLE_CONNECT) return 0;
    int current = s->hello && !memcmp(m->from, s->x.peer_session, 16);
    if (m->kind == X_OFFER && !current && !(m->flags & X_HAVE)) {
        listen_offer(s, m);
        return 1;
    }
    if (!current) {
        /* a connect end that lost its session hears so at once, instead of after its liveness timeout */
        if (m->kind == X_PING || m->kind == X_OFFER) xchg_answer(&s->x, m->from, X_BYE, ROLE_LISTEN, NULL);
        return 0;
    }
    s->heard = now_ns();
    if (m->kind == X_OFFER) {
        if ((m->flags & X_HAVE) && !s->armed) {
            s->armed = 1;
            s->since = (long long)time(NULL);
            logf_("%s: peer ready over %s (%s replies, exchange on %s)", s->name, link_kind_name(s->e.kind),
                  s->direct ? "direct" : "pull", s->x.ifname);
        }
        if (!s->armed) (void)xchg_send(&s->x, &s->answer);
    } else if (m->kind == X_PING) {
        xchg_answer(&s->x, s->x.peer_session, X_PONG, ROLE_LISTEN, NULL);
    } else if (m->kind == X_BYE || m->kind == X_ERR) {
        listen_drop(s, m->kind == X_ERR ? m->text : "the connect end said goodbye");
    }
    return 0;
}

/* Send the staged reply: direct mode writes it and the client's done word, pull mode only the ready word. */
static int listen_reply(struct listen_state *s, uint32_t seq, uint32_t len) {
    uint64_t req = s->b.req;
    store_word((volatile uint64_t *)(s->b.base + req), WORD(seq, len));
    uint64_t began = now_ns();
    int bad = s->direct ? box_transfer(&s->e, &s->b, &s->remote, req + CTRL, len, req, req + 64, WORD(seq, len),
                                       s->pieces, PIECES, WINDOW_PEER)
                        : box_transfer(&s->e, &s->b, &s->remote, req, 0, req, req, WORD(seq, len), s->pieces, PIECES,
                                       WINDOW_PEER);
    if (bad) return -1;
    if (len >= BULK && s->e.kind == LINK_ROCE) log_rate(s->name, "reply", len, now_ns() - began);
    s->served++;
    s->bytes += len;
    return 0;
}

/* The link's mailbox: a file under MCDMA_RPC_BOX_DIR (Linux /dev/shm, or a test directory), else POSIX shm. */
static int open_box(const char *path, uint64_t total, const struct owner *owner) {
#ifdef MCDMA_RPC_BOX_DIR
    /* open before create: root may not O_CREAT over the service user's file in sticky /dev/shm */
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || fchmod(fd, 0600) || (owner->set && fchown(fd, owner->uid, owner->gid)) || ftruncate(fd, 0) ||
        ftruncate(fd, (off_t)total)) {
#else
    (void)owner;
    shm_unlink(path);
    int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)total)) {
#endif
        logf_("mailbox %s errno=%d", path, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void remove_box(const char *path) {
#ifdef MCDMA_RPC_BOX_DIR
    unlink(path);
#else
    shm_unlink(path);
#endif
}

int run_listen(const char *name, const char *device, int gid_index, int mtu, const char *via, int port,
               uint64_t req_bytes, uint64_t rep_bytes, const struct owner *owner) {
    static struct listen_state s;
    static struct reader srd, prd[MAX_PENDING];
    uint64_t pending_since[MAX_PENDING] = {0};
    s.name = name;
    s.svc = -1;
    s.x.fd = -1;
    char default_sock[128], box_path[160];
    snprintf(default_sock, sizeof(default_sock), "%s/mcdma-rpcd.%s.sock", MCDMA_RPC_SOCK_DIR, name);
#ifdef MCDMA_RPC_BOX_DIR
    snprintf(box_path, sizeof(box_path), "%s/mcdma-rpc.%s", MCDMA_RPC_BOX_DIR, name);
#else
    snprintf(box_path, sizeof(box_path), "/mcdma-rpc.%s", name);
#endif
    const char *sock_path = socket_path(default_sock);
    char host[64];
    if (link_host(name, host, sizeof(host)) < 0) {
        logf_("MCDMA_RPCD_HOSTS must be NAME=HOST[,NAME=HOST]");
        return 2;
    }
    /* the lock comes first: a second daemon for this link must leave the live mailbox alone */
    if (hold_link_lock(name, MCDMA_RPC_LISTEN_LOCK) < 0) return 2;
    if (socket_in_use(sock_path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", sock_path);
        return 2;
    }
    int us = unix_listen(sock_path);
    if (us >= 0 && owner->set && chown(sock_path, owner->uid, owner->gid)) {
        logf_("chown %s errno=%d", sock_path, errno);
        close(us);
        us = -1;
    }
    if (us < 0 || xchg_open(&s.x, via, port, 0, name)) {
        if (us >= 0) close(us);
        unlink(sock_path);
        return 2;
    }
    s.b.req = req_bytes;
    s.b.rep = rep_bytes;
    uint64_t total = s.b.req + s.b.rep;
    int bf = -1;
    if (ep_open(&s.e, device, gid_index, mtu) || via_check(via, &s.e) || (bf = open_box(box_path, total, owner)) < 0)
        goto fail;
    s.b.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, bf, 0);
    close(bf);
    if (s.b.base == MAP_FAILED) {
        s.b.base = NULL;
        logf_("mmap mailbox");
        goto fail;
    }
    memset(s.b.base, 0, total);
    write_sizes(&s.b);
    if (box_register(&s.e, &s.b, 1)) {
        logf_("register mailbox");
        goto fail;
    }
    volatile uint64_t *staged_word = (volatile uint64_t *)(s.b.base + s.b.req + 128);
    logf_("listen %s: %s (%s) gid %d mtu %d, exchange %s port %d, socket %s, mailbox %s (%" PRIu64 " + %" PRIu64 " MiB)",
          name, device, link_kind_name(s.e.kind), gid_index, mtu, via, s.x.port, sock_path, box_path, s.b.req >> 20,
          s.b.rep >> 20);

    uint32_t last_staged = 0;
    uint64_t active = now_ns();
    char *line = malloc(LINE);
    for (int i = 0; i < MAX_PENDING; ++i) prd[i].fd = -1;
    while (!g_stop && line) {
        struct pollfd fds[3 + MAX_PENDING];
        int nf = 0;
        fds[nf++] = (struct pollfd){.fd = s.x.fd, .events = POLLIN};
        fds[nf++] = (struct pollfd){.fd = us, .events = POLLIN};
        fds[nf++] = (struct pollfd){.fd = s.svc, .events = POLLIN};
        for (int i = 0; i < MAX_PENDING; ++i) fds[nf++] = (struct pollfd){.fd = prd[i].fd, .events = POLLIN};
        int busy = s.armed && s.svc >= 0, waited = poll(fds, (nfds_t)nf, busy ? 0 : s.hello && !s.armed ? 20 : 200);
        if (waited < 0 && errno != EINTR) break;
        struct xmsg m;
        while (xchg_recv(&s.x, &m, 0) == 1)
            if (listen_datagram(&s, &m)) last_staged = 0;
        /* read after the datagrams, which may have started a session or heard the peer just now */
        uint64_t now = now_ns();
        if (fds[1].revents & POLLIN) {
            int fd = accept(us, NULL, NULL);
            int slot = -1;
            for (int i = 0; i < MAX_PENDING && fd >= 0; ++i)
                if (prd[i].fd < 0) {
                    slot = i;
                    break;
                }
            if (fd >= 0 && slot < 0) close(fd);
            if (slot >= 0) {
                set_io_timeout(fd, IO_TIMEOUT_S);
                memset(&prd[slot], 0, sizeof(prd[slot]));
                prd[slot].fd = fd;
                pending_since[slot] = now;
            }
        }
        for (int i = 0; i < MAX_PENDING; ++i) {
            if (prd[i].fd < 0) continue;
            int got = take_line(&prd[i], line, LINE, 0);
            /* a client that never sends its command gives its slot up, so SHUTDOWN and STATUS always get in */
            if (got == 0 && now - pending_since[i] <= PENDING_NS) continue;
            int fd = prd[i].fd;
            prd[i].fd = -1;
            if (got <= 0) {
                close(fd);
            } else if (!strcmp(line, "MODE poll")) {
                if (s.svc >= 0) {
                    send_line(fd, "ERR busy");
                    close(fd);
                } else if (send_line(fd, "OK")) {
                    close(fd);
                } else {
                    s.svc = fd;
                    memset(&srd, 0, sizeof(srd));
                    srd.fd = fd;
                    last_staged = WORD_SEQ(load_word(staged_word));
                    logf_("listen %s: service attached", name);
                }
            } else if (!strcmp(line, "STATUS")) {
                listen_status(&s, fd);
                close(fd);
            } else if (!strcmp(line, "SHUTDOWN")) {
                send_line(fd, "BYE");
                close(fd);
                g_stop = 1;
            } else {
                send_line(fd, "ERR unknown command");
                close(fd);
            }
        }
        if (s.hello && !s.armed && now - s.hello_since > HANDSHAKE_NS) {
            /* a session that never confirms must not hold the link */
            logf_("listen %s: no confirmation within %d s; dropping the session", name, MCDMA_RPC_HANDSHAKE_S);
            listen_drop(&s, "handshake timeout");
        } else if (s.hello && !s.armed && now - s.offered > RESEND_NS) {
            (void)xchg_send(&s.x, &s.answer);
            s.offered = now;
        } else if (s.armed && now - s.heard > LIVENESS_NS) {
            listen_drop(&s, "the connect end is silent");
        }
        if (s.svc >= 0 && take_line(&srd, line, LINE, 0) < 0) {
            logf_("listen %s: service detached", name);
            close(s.svc);
            s.svc = -1;
        }
        /* Thunderbolt requests land only when this end posts receives for them */
        if (s.hello && s.e.kind == LINK_TB) {
            int handled = tb_progress(&s.e);
            if (handled < 0) {
                s.failures++;
                listen_drop(&s, "thunderbolt link failed");
            } else if (handled) {
                active = now_ns();
            }
        }
        if (s.svc >= 0 && s.armed) {
            uint64_t sw = load_word(staged_word);
            uint32_t seq = WORD_SEQ(sw), len = WORD_LEN(sw);
            if (seq && seq != last_staged) {
                last_staged = seq;
                active = now_ns();
                if (len > s.b.rep - CTRL || listen_reply(&s, seq, len)) {
                    s.failures++;
                    logf_("listen %s: reply write failed; dropping the link", name);
                    listen_drop(&s, "reply write");
                }
            }
        }
        if (busy && !waited) {
            if (now_ns() - active > 50000000ull)
                usleep(20);
            else
                cpu_relax();
        }
    }
    listen_drop(&s, "shutdown");
    if (s.svc >= 0) {
        send_line(s.svc, "BYE");
        close(s.svc);
    }
    for (int i = 0; i < MAX_PENDING; ++i)
        if (prd[i].fd >= 0) close(prd[i].fd);
    free(line);
    xchg_close(&s.x);
    close(us);
    unlink(sock_path);
    ep_close(&s.e);
    munmap(s.b.base, total);
    remove_box(box_path);
    logf_("listen %s: every verbs object destroyed, exiting", name);
    return 0;

fail:
    xchg_close(&s.x);
    close(us);
    unlink(sock_path);
    ep_close(&s.e);
    if (s.b.base) munmap(s.b.base, total);
    /* the link lock is ours, so whatever mailbox exists belongs to this failed start */
    remove_box(box_path);
    return 2;
}
