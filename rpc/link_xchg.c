/* The setup exchange: offers, liveness and goodbyes as IPv6 link-local datagrams on one Thunderbolt IP interface,
 * never TCP. A datagram is admitted only from a link-local source, on that interface, with hop limit 255; routers
 * forward none of those, so on a point-to-point Thunderbolt cable only the machine at the other end is admitted. */
#include "link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define X_MAGIC "MCDX"
#define X_VERSION 1
#define X_HEAD 64
#define X_OFFER_FIXED 144

static void all_nodes(struct in6_addr *a) {
    memset(a, 0, sizeof(*a));
    a->s6_addr[0] = 0xff;
    a->s6_addr[1] = 0x02;
    a->s6_addr[15] = 1;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

int xchg_encode(const struct xmsg *m, uint8_t *out, size_t cap) {
    size_t n = X_HEAD;
    if (m->kind == X_OFFER) {
        if (m->info.table.n > LINK_REGION_MAX) return -1;
        n = X_OFFER_FIXED + 4u * m->info.table.n;
    } else if (m->kind == X_ERR) {
        n = X_HEAD + sizeof(m->text);
    }
    if (n > cap || n > X_MAX || !memchr(m->name, 0, X_NAME)) return -1;
    memset(out, 0, n);
    memcpy(out, X_MAGIC, 4);
    out[4] = X_VERSION, out[5] = m->kind, out[6] = m->role, out[7] = m->flags;
    memcpy(out + 8, m->name, X_NAME);
    memcpy(out + 32, m->from, 16);
    memcpy(out + 48, m->to, 16);
    if (m->kind == X_OFFER) {
        const struct xinfo *i = &m->info;
        out[64] = i->transport, out[65] = i->mode;
        put16(out + 66, i->lid);
        put32(out + 68, i->qpn), put32(out + 72, i->qpn2), put32(out + 76, i->psn), put32(out + 80, i->frames);
        memcpy(out + 84, i->gid, 16);
        put64(out + 100, i->req), put64(out + 108, i->rep);
        put64(out + 116, i->table.base), put64(out + 124, i->table.length), put64(out + 132, i->table.seg);
        put32(out + 140, i->table.n);
        for (uint32_t k = 0; k < i->table.n; ++k) put32(out + X_OFFER_FIXED + 4 * k, i->table.rkey[k]);
    } else if (m->kind == X_ERR) {
        memcpy(out + X_HEAD, m->text, sizeof(m->text));
        out[X_HEAD + sizeof(m->text) - 1] = 0;
    }
    return (int)n;
}

int xchg_decode(const uint8_t *in, size_t len, struct xmsg *m) {
    memset(m, 0, sizeof(*m));
    if (len < X_HEAD || len > X_MAX || memcmp(in, X_MAGIC, 4) || in[4] != X_VERSION) return -1;
    m->kind = in[5], m->role = in[6], m->flags = in[7];
    if (m->kind < X_OFFER || m->kind > X_ERR || m->role < ROLE_CONNECT || m->role > ROLE_PEER) return -1;
    memcpy(m->name, in + 8, X_NAME);
    if (!memchr(m->name, 0, X_NAME)) return -1;
    memcpy(m->from, in + 32, 16);
    memcpy(m->to, in + 48, 16);
    if (m->kind == X_OFFER) {
        if (len < X_OFFER_FIXED) return -1;
        struct xinfo *i = &m->info;
        i->transport = in[64], i->mode = in[65], i->lid = get16(in + 66);
        i->qpn = get32(in + 68), i->qpn2 = get32(in + 72), i->psn = get32(in + 76), i->frames = get32(in + 80);
        memcpy(i->gid, in + 84, 16);
        i->req = get64(in + 100), i->rep = get64(in + 108);
        i->table.base = get64(in + 116), i->table.length = get64(in + 124), i->table.seg = get64(in + 132);
        i->table.n = get32(in + 140);
        if (i->table.n > LINK_REGION_MAX || len != X_OFFER_FIXED + 4u * i->table.n) return -1;
        if (i->transport != LINK_ROCE && i->transport != LINK_TB) return -1;
        for (uint32_t k = 0; k < i->table.n; ++k) i->table.rkey[k] = get32(in + X_OFFER_FIXED + 4 * k);
    } else if (m->kind == X_ERR) {
        if (len != X_HEAD + sizeof(m->text)) return -1;
        memcpy(m->text, in + X_HEAD, sizeof(m->text));
        if (!memchr(m->text, 0, sizeof(m->text))) return -1;
    } else if (len != X_HEAD) {
        return -1;
    }
    return 0;
}

/* IFACE, IFACE/fe80::ADDR or IFACE/A.B.C.D (a link whose ends have only IPv4, such as a Thunderbolt /30); the address,
 * when given, is the only peer admitted and is written without a %zone. An IPv4 peer is held as ::ffff:A.B.C.D. */
int via_parse(const char *via, char *ifname, size_t n, struct in6_addr *peer, int *pinned) {
    const char *slash = strchr(via, '/');
    size_t len = slash ? (size_t)(slash - via) : strlen(via);
    *pinned = 0;
    memset(peer, 0, sizeof(*peer));
    if (!len || len >= n || len >= IFNAMSIZ) return -1;
    memcpy(ifname, via, len);
    ifname[len] = 0;
    if (!slash) return 0;
    struct in_addr v4;
    if (!strchr(slash + 1, '%') && inet_pton(AF_INET, slash + 1, &v4) == 1) {
        peer->s6_addr[10] = peer->s6_addr[11] = 0xff;
        memcpy(&peer->s6_addr[12], &v4, 4);
        *pinned = 1;
        return 0;
    }
    if (strchr(slash + 1, '%') || inet_pton(AF_INET6, slash + 1, peer) != 1 || !IN6_IS_ADDR_LINKLOCAL(peer)) return -1;
    *pinned = 1;
    return 0;
}

/* The pinned IPv4 peer's address as a socket address. */
static struct sockaddr_in v4_peer(const struct xchg *x, int port) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    memcpy(&to.sin_addr, &x->peer.s6_addr[12], 4);
    return to;
}

/* IPv4: a socket bound to the interface where it can be, sending with TTL 255 and reporting each datagram's
 * interface and TTL, so only the pinned peer one hop away on this link is admitted, as on IPv6. */
static int open_v4(struct xchg *x, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, ttl = 255;
    unsigned idx = x->ifindex;
    int ok = fd >= 0 && !fcntl(fd, F_SETFD, FD_CLOEXEC) &&
             !setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) &&
             !setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one)) &&
             !setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
#ifdef IP_BOUND_IF
    ok = ok && !setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx));
#else
    (void)idx;
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t alen = sizeof(a);
    if (!ok || bind(fd, (struct sockaddr *)&a, sizeof(a)) || getsockname(fd, (struct sockaddr *)&a, &alen)) {
        link_log("exchange socket on %s port %d (IPv4) errno=%d", x->ifname, port, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    x->fd = fd;
    x->port = ntohs(a.sin_port);
    return 0;
}

void xchg_session(struct xchg *x) {
    link_random(x->session, sizeof(x->session));
    memset(x->peer_session, 0, sizeof(x->peer_session));
}

int xchg_open(struct xchg *x, const char *via, int port, int peer_port, const char *name) {
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    if (port < 0 || port > 65535 || peer_port < 0 || peer_port > 65535 || strlen(name) >= X_NAME) return -1;
    if (via_parse(via, x->ifname, sizeof(x->ifname), &x->peer, &x->pinned)) {
        link_log("bad exchange interface %s: give IFACE or IFACE/fe80::ADDR", via);
        return -1;
    }
    if (!(x->ifindex = if_nametoindex(x->ifname))) {
        link_log("no interface %s for the exchange", x->ifname);
        return -1;
    }
    snprintf(x->name, sizeof(x->name), "%s", name);
    x->port = port;
    x->peer_port = peer_port ? peer_port : port;
    x->v4 = x->pinned && IN6_IS_ADDR_V4MAPPED(&x->peer);
    if (x->v4) {
        if (open_v4(x, port)) return -1;
        xchg_session(x);
        return 0;
    }
    int fd = socket(AF_INET6, SOCK_DGRAM, 0), one = 1, hops = 255;
    unsigned idx = x->ifindex;
    int ok = fd >= 0 && !fcntl(fd, F_SETFD, FD_CLOEXEC) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) &&
             !setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
#ifdef IPV6_BOUND_IF
    ok = ok && !setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &idx, sizeof(idx));
#endif
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((uint16_t)port);
    a.sin6_addr = in6addr_any;
    socklen_t alen = sizeof(a);
    if (!ok || bind(fd, (struct sockaddr *)&a, sizeof(a)) || getsockname(fd, (struct sockaddr *)&a, &alen)) {
        link_log("exchange socket on %s port %d errno=%d", x->ifname, port, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    struct ipv6_mreq join;
    all_nodes(&join.ipv6mr_multiaddr);
    join.ipv6mr_interface = idx;
    /* every node is already in ff02::1; joining makes sure this socket receives it on every system */
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &join, sizeof(join));
    x->fd = fd;
    x->port = ntohs(a.sin6_port);
    xchg_session(x);
    return 0;
}

void xchg_close(struct xchg *x) {
    if (x->fd >= 0) close(x->fd);
    x->fd = -1;
}

void xchg_prepare(const struct xchg *x, struct xmsg *m, int kind, int role, int flags) {
    memset(m, 0, sizeof(*m));
    m->kind = (uint8_t)kind, m->role = (uint8_t)role, m->flags = (uint8_t)flags;
    snprintf(m->name, sizeof(m->name), "%s", x->name);
    memcpy(m->from, x->session, 16);
    memcpy(m->to, x->peer_session, 16);
}

int xchg_send(struct xchg *x, const struct xmsg *m) {
    uint8_t buf[X_MAX];
    int n = xchg_encode(m, buf, sizeof(buf));
    if (n < 0 || x->fd < 0) return -1;
    if (x->v4) {
        struct sockaddr_in to4 = v4_peer(x, x->peer_port);
        return sendto(x->fd, buf, (size_t)n, 0, (struct sockaddr *)&to4, sizeof(to4)) == n ? 0 : -1;
    }
    struct sockaddr_in6 to;
    memset(&to, 0, sizeof(to));
    to.sin6_family = AF_INET6;
    to.sin6_port = htons((uint16_t)x->peer_port);
    if (x->learned || x->pinned) to.sin6_addr = x->peer;
    else all_nodes(&to.sin6_addr);
    to.sin6_scope_id = x->ifindex;
    return sendto(x->fd, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to)) == n ? 0 : -1;
}

static void reject(struct xchg *x, const char *why, const struct sockaddr_in6 *src) {
    uint64_t now = link_now_ns();
    x->rejected++;
    if (x->warned_ns && now - x->warned_ns < 10000000000ull) return;
    x->warned_ns = now;
    char addr[INET6_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET6, &src->sin6_addr, addr, sizeof(addr));
    link_log("%s: ignored a datagram from %s on %s: %s (%llu so far)", x->name, addr, x->ifname, why,
             (unsigned long long)x->rejected);
}

/* The IPv4 receive: only the pinned peer, on this interface, one hop away (TTL 255). */
static int recv_v4(struct xchg *x, struct xmsg *m) {
    uint8_t buf[X_MAX + 1];
    struct sockaddr_in src;
    union {
        struct cmsghdr align;
        uint8_t raw[256];
    } control;
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr h;
    memset(&h, 0, sizeof(h));
    memset(&src, 0, sizeof(src));
    h.msg_name = &src, h.msg_namelen = sizeof(src);
    h.msg_iov = &iov, h.msg_iovlen = 1;
    h.msg_control = control.raw, h.msg_controllen = sizeof(control.raw);
    ssize_t got = recvmsg(x->fd, &h, MSG_DONTWAIT);
    if (got < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    int ttl = -1;
    unsigned arrived = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
        if (c->cmsg_level != IPPROTO_IP) continue;
        if (c->cmsg_type == IP_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo info;
            memcpy(&info, CMSG_DATA(c), sizeof(info));
            arrived = (unsigned)info.ipi_ifindex;
#ifdef __APPLE__
        } else if (c->cmsg_type == IP_RECVTTL && c->cmsg_len >= CMSG_LEN(sizeof(unsigned char))) {
            ttl = *(const unsigned char *)CMSG_DATA(c);
#else
        } else if (c->cmsg_type == IP_TTL && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&ttl, CMSG_DATA(c), sizeof(ttl));
#endif
        }
    }
    struct sockaddr_in want = v4_peer(x, 0);
    struct sockaddr_in6 shown;
    memset(&shown, 0, sizeof(shown));
    shown.sin6_family = AF_INET6;
    shown.sin6_addr = x->peer;
    memcpy(&shown.sin6_addr.s6_addr[12], &src.sin_addr, 4);
    const char *why = NULL;
    if (src.sin_family != AF_INET || memcmp(&src.sin_addr, &want.sin_addr, 4)) why = "not the configured peer";
    else if (arrived != x->ifindex) why = "arrived on another interface";
    else if (ttl != 255) why = "TTL is not 255, so it did not come from this link";
    else if ((size_t)got > X_MAX || xchg_decode(buf, (size_t)got, m)) why = "not an MCDMA exchange message";
    else if (strncmp(m->name, x->name, X_NAME)) why = "names another link";
    if (why) {
        reject(x, why, &shown);
        return 0;
    }
    if (!memcmp(m->from, x->session, 16)) return 0;
    x->peer_port = ntohs(src.sin_port);
    x->learned = 1;
    return 1;
}

/* 1 and an admitted message, 0 when nothing admissible arrived within timeout_ms, -1 on a socket error. */
int xchg_recv(struct xchg *x, struct xmsg *m, int timeout_ms) {
    struct pollfd wait = {.fd = x->fd, .events = POLLIN};
    int ready = poll(&wait, 1, timeout_ms);
    if (ready <= 0) return ready < 0 && errno != EINTR ? -1 : 0;
    if (x->v4) return recv_v4(x, m);
    uint8_t buf[X_MAX + 1];
    struct sockaddr_in6 src;
    union {
        struct cmsghdr align;
        uint8_t raw[256];
    } control;
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr h;
    memset(&h, 0, sizeof(h));
    memset(&src, 0, sizeof(src));
    h.msg_name = &src, h.msg_namelen = sizeof(src);
    h.msg_iov = &iov, h.msg_iovlen = 1;
    h.msg_control = control.raw, h.msg_controllen = sizeof(control.raw);
    ssize_t got = recvmsg(x->fd, &h, MSG_DONTWAIT);
    if (got < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    int hops = -1;
    unsigned arrived = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
        if (c->cmsg_level != IPPROTO_IPV6) continue;
        if (c->cmsg_type == IPV6_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(c), sizeof(info));
            arrived = info.ipi6_ifindex;
        } else if (c->cmsg_type == IPV6_HOPLIMIT && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&hops, CMSG_DATA(c), sizeof(hops));
        }
    }
    const char *why = NULL;
    if (src.sin6_family != AF_INET6 || !IN6_IS_ADDR_LINKLOCAL(&src.sin6_addr)) why = "source is not link-local";
    else if (arrived != x->ifindex) why = "arrived on another interface";
    else if (hops != 255) why = "hop limit is not 255, so it did not come from this link";
    else if (x->pinned && memcmp(&src.sin6_addr, &x->peer, sizeof(x->peer))) why = "not the configured peer";
    else if ((size_t)got > X_MAX || xchg_decode(buf, (size_t)got, m)) why = "not an MCDMA exchange message";
    else if (strncmp(m->name, x->name, X_NAME)) why = "names another link";
    if (why) {
        reject(x, why, &src);
        return 0;
    }
    /* our own multicast offer, looped back */
    if (!memcmp(m->from, x->session, 16)) return 0;
    x->peer = src.sin6_addr;
    x->peer_port = ntohs(src.sin6_port);
    x->learned = 1;
    return 1;
}

int xchg_ours(const struct xchg *x, const struct xmsg *m) { return !memcmp(m->to, x->session, 16); }

/* On a Mac the exchange runs over Thunderbolt, whose RDMA devices Apple names after their ports (rdma_en2, en2).
 * A ConnectX-5 link may also meet on another interface, such as the LAN, when the peer's address is pinned: its
 * payload can only arrive over the CX5's own cable, so a forged datagram there can stop the link coming up but cannot
 * reach memory. This is for peers with no Thunderbolt networking, such as a DGX Spark. */
int via_check(const char *via, const struct ep *e) {
#if defined(__APPLE__) && !defined(MCDMA_LINK_TEST_INTERFACES)
    char ifname[32], want[48];
    struct in6_addr pin;
    int pinned = 0, n = 0, found = 0;
    if (via_parse(via, ifname, sizeof(ifname), &pin, &pinned)) return -1;
    snprintf(want, sizeof(want), "rdma_%s", ifname);
    if (e->kind == LINK_TB && strcmp(e->device, want)) {
        link_log("%s runs on its own Thunderbolt port, not %s: give that port as the exchange interface", e->device,
                 ifname);
        return -1;
    }
    struct ibv_device **list = ibv_get_device_list(&n);
    for (int i = 0; list && i < n && !found; ++i) found = !strcmp(ibv_get_device_name(list[i]), want);
    if (list) ibv_free_device_list(list);
    if (!found && e->kind == LINK_ROCE && pinned) {
        link_log("%s meets on %s, not Thunderbolt, admitting only the pinned peer", e->device, via);
        return 0;
    }
    if (!found && e->kind == LINK_ROCE)
        link_log("%s is not a Thunderbolt port with RDMA enabled (no %s): give it as IFACE/PEER-ADDRESS to meet a "
                 "ConnectX peer there", ifname, want);
    else if (!found)
        link_log("%s is not a Thunderbolt port with RDMA enabled (no %s): the exchange runs only over Thunderbolt",
                 ifname, want);
    return found ? 0 : -1;
#else
    (void)via, (void)e;
    return 0;
#endif
}
