/* Offline test of the setup exchange: datagram encoding, interface parsing, and admission over the loopback
 * interface's link-local address (lo0 on macOS; skipped where loopback has none). Nothing leaves the host. */
#include "../rpc/link.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_logged;

void link_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_logged++;
    if (getenv("TEST_VERBOSE")) vfprintf(stderr, fmt, ap), fputc('\n', stderr);
    va_end(ap);
}

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "test_link_xchg: %s\n", what);
        _exit(1);
    }
}

static void offer(struct xmsg *m, uint32_t keys) {
    memset(m, 0, sizeof(*m));
    m->kind = X_OFFER, m->role = ROLE_CONNECT, m->flags = X_HAVE;
    snprintf(m->name, sizeof(m->name), "worker-a");
    for (int i = 0; i < 16; ++i) m->from[i] = (uint8_t)(i + 1), m->to[i] = (uint8_t)(0xf0 | i);
    struct xinfo *i = &m->info;
    i->transport = LINK_TB, i->mode = MODE_DIRECT, i->lid = 1, i->qpn = 0x123456, i->qpn2 = 0x654321, i->psn = 0xabcdef;
    i->frames = 4095, i->req = 4ull << 20, i->rep = 64ull << 20;
    for (int k = 0; k < 16; ++k) i->gid[k] = (uint8_t)(0xa0 + k);
    i->table.base = 0x1234567890ull, i->table.length = 68ull << 20, i->table.seg = 4ull << 20, i->table.n = keys;
    for (uint32_t k = 0; k < keys; ++k) i->table.rkey[k] = 0x10000 + k;
}

static void codec(void) {
    uint8_t buf[X_MAX + 64];
    struct xmsg m, back;
    offer(&m, LINK_REGION_MAX);
    int n = xchg_encode(&m, buf, sizeof(buf));
    check(n > 0 && n <= X_MAX, "an offer with every key fits one datagram");
    check(!xchg_decode(buf, (size_t)n, &back), "the offer decodes");
    check(!memcmp(&m, &back, sizeof(m)), "the offer round-trips exactly");
    uint8_t bad[X_MAX + 64];
    const struct {
        size_t at;
        uint8_t value;
        const char *what;
    } flips[] = {{0, 'X', "magic"}, {4, 9, "version"}, {5, 0, "kind"}, {5, 9, "kind"}, {6, 7, "role"}, {64, 3, "transport"},
                 {140, 129, "key count"}};
    for (size_t k = 0; k < sizeof(flips) / sizeof(flips[0]); ++k) {
        memcpy(bad, buf, (size_t)n);
        bad[flips[k].at] = flips[k].value;
        check(xchg_decode(bad, (size_t)n, &back) != 0, flips[k].what);
    }
    memcpy(bad, buf, (size_t)n);
    memset(bad + 8, 'a', X_NAME);
    check(xchg_decode(bad, (size_t)n, &back) != 0, "a name without its terminator is refused");
    check(xchg_decode(buf, (size_t)n - 1, &back) != 0, "a truncated offer is refused");
    check(xchg_decode(buf, 63, &back) != 0, "a short header is refused");
    memset(&m, 0, sizeof(m));
    m.kind = X_ERR, m.role = ROLE_LISTEN;
    snprintf(m.name, sizeof(m.name), "w");
    snprintf(m.text, sizeof(m.text), "mailbox sizes differ");
    n = xchg_encode(&m, buf, sizeof(buf));
    check(n > 0 && !xchg_decode(buf, (size_t)n, &back) && !strcmp(back.text, "mailbox sizes differ"),
          "an error round-trips");
    m.kind = X_PING;
    n = xchg_encode(&m, buf, sizeof(buf));
    check(n == 64 && !xchg_decode(buf, (size_t)n, &back) && back.kind == X_PING, "a ping is the bare header");
    check(xchg_decode(buf, 65, &back) != 0, "a ping with a body is refused");
}

static void parsing(void) {
    char ifname[32];
    struct in6_addr peer;
    int pinned = 0;
    check(!via_parse("en2", ifname, sizeof(ifname), &peer, &pinned) && !pinned && !strcmp(ifname, "en2"), "IFACE");
    check(!via_parse("en2/fe80::1021:1bac:fe1:b5e3", ifname, sizeof(ifname), &peer, &pinned) && pinned &&
              peer.s6_addr[0] == 0xfe && peer.s6_addr[15] == 0xe3,
          "IFACE/fe80::ADDR pins the peer");
    check(!via_parse("en2/192.0.2.1", ifname, sizeof(ifname), &peer, &pinned) && pinned &&
              IN6_IS_ADDR_V4MAPPED(&peer) && peer.s6_addr[12] == 192 && peer.s6_addr[15] == 1,
          "IFACE/IPv4 pins the peer");
    const char *bad[] = {"", "/fe80::1", "en2/::1", "en2/192.0.2.999", "en2/fe80::1%en2", "en2/2001:db8::1",
                         "an-interface-name-far-too-long-for-any-system"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k)
        check(via_parse(bad[k], ifname, sizeof(ifname), &peer, &pinned) != 0, bad[k]);
}

static const char *loopback(void) {
    struct ifaddrs *all = NULL;
    const char *name = NULL;
    if (getifaddrs(&all)) return NULL;
    for (struct ifaddrs *a = all; a && !name; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET6 && (a->ifa_flags & IFF_LOOPBACK) &&
            IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr))
            name = !strcmp(a->ifa_name, "lo0") ? "lo0" : "lo";
    freeifaddrs(all);
    return name;
}

/* Send raw bytes to `port` on the loopback from `src` with `hops`, as a spoofing or off-link sender would. */
static void raw(const char *ifname, const char *src, int port, int hops, const uint8_t *buf, int n) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    unsigned idx = if_nametoindex(ifname);
    struct sockaddr_in6 to, from;
    memset(&to, 0, sizeof(to));
    memset(&from, 0, sizeof(from));
    to.sin6_family = from.sin6_family = AF_INET6;
    to.sin6_port = htons((uint16_t)port);
    inet_pton(AF_INET6, strcmp(src, "::1") ? "fe80::1" : "::1", &to.sin6_addr);
    to.sin6_scope_id = strcmp(src, "::1") ? idx : 0;
    inet_pton(AF_INET6, src, &from.sin6_addr);
    from.sin6_scope_id = to.sin6_scope_id;
    setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
    check(!bind(fd, (struct sockaddr *)&from, sizeof(from)), "raw sender binds");
    check(sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to)) == n, "raw send");
    close(fd);
}

static void admission(const char *lo) {
    struct xchg listen, conn, pinned_other;
    struct xmsg m, got;
    check(!xchg_open(&listen, lo, 0, 0, "worker-a"), "the listen end opens on an ephemeral port");
    check(!xchg_open(&conn, lo, 0, listen.port, "worker-a"), "the connect end opens");
    check(xchg_recv(&listen, &got, 50) == 0, "nothing has arrived yet");
    /* first contact goes to ff02::1 on the interface, which only the cable's other end receives */
    xchg_prepare(&conn, &m, X_OFFER, ROLE_CONNECT, 0);
    offer(&m, 0);
    memcpy(m.from, conn.session, 16);
    memset(m.to, 0, 16);
    check(!conn.learned && !xchg_send(&conn, &m), "a multicast offer is sent");
    check(xchg_recv(&listen, &got, 1000) == 1 && got.kind == X_OFFER && !memcmp(got.from, conn.session, 16),
          "the listen end admits a link-local, hop-limit-255 offer");
    check(listen.learned && listen.peer_port == conn.port, "the listen end learns where to answer");
    offer(&m, 3);
    m.role = ROLE_LISTEN;
    memcpy(m.from, listen.session, 16);
    memcpy(m.to, got.from, 16);
    check(!xchg_send(&listen, &m), "an answer is sent");
    check(xchg_recv(&conn, &got, 1000) == 1 && xchg_ours(&conn, &got) && got.role == ROLE_LISTEN, "the answer is admitted");
    uint8_t buf[X_MAX];
    xchg_prepare(&conn, &m, X_PING, ROLE_CONNECT, 0);
    int n = xchg_encode(&m, buf, sizeof(buf));
    int before = g_logged;
    raw(lo, "fe80::1", listen.port, 64, buf, n);
    check(xchg_recv(&listen, &got, 300) == 0, "a datagram with hop limit 64 is ignored");
    raw(lo, "::1", listen.port, 255, buf, n);
    check(xchg_recv(&listen, &got, 300) == 0, "a datagram from a non-link-local source is ignored");
    raw(lo, "fe80::1", listen.port, 255, buf, n);
    check(xchg_recv(&listen, &got, 300) == 1 && got.kind == X_PING, "the same datagram from on-link is admitted");
    xchg_prepare(&conn, &m, X_PING, ROLE_CONNECT, 0);
    snprintf(m.name, sizeof(m.name), "worker-b");
    n = xchg_encode(&m, buf, sizeof(buf));
    raw(lo, "fe80::1", listen.port, 255, buf, n);
    check(xchg_recv(&listen, &got, 300) == 0, "another link's datagram is ignored");
    raw(lo, "fe80::1", listen.port, 255, (const uint8_t *)"not an exchange message", 23);
    check(xchg_recv(&listen, &got, 300) == 0, "garbage is ignored");
    check(g_logged > before && listen.rejected == 4, "ignored datagrams are counted and logged");
    char via[64];
    snprintf(via, sizeof(via), "%s/fe80::2", lo);
    check(!xchg_open(&pinned_other, via, 0, 0, "worker-a"), "a pinned exchange opens");
    xchg_prepare(&conn, &m, X_PING, ROLE_CONNECT, 0);
    n = xchg_encode(&m, buf, sizeof(buf));
    raw(lo, "fe80::1", pinned_other.port, 255, buf, n);
    check(xchg_recv(&pinned_other, &got, 300) == 0, "a pinned end ignores every other address");
    struct xchg nowhere;
    check(xchg_open(&nowhere, "no-such-if0", 0, 0, "worker-a") != 0, "a missing interface is refused");
    xchg_close(&listen), xchg_close(&conn), xchg_close(&pinned_other);
}

/* The socket listens on its interface alone: bound to it, not only filtering what arrives there. */
static void bound(const char *ifname) {
    struct xchg x;
    check(!xchg_open(&x, ifname, 0, 0, "worker-a"), "an exchange opens on the loopback");
#if defined(IPV6_BOUND_IF)
    unsigned idx = 0;
    socklen_t len = sizeof(idx);
    check(!getsockopt(x.fd, IPPROTO_IPV6, IPV6_BOUND_IF, &idx, &len) && idx == if_nametoindex(ifname),
          "the exchange socket is bound to its interface");
#elif defined(SO_BINDTODEVICE)
    char dev[IF_NAMESIZE + 1] = {0};
    socklen_t len = sizeof(dev);
    check(!getsockopt(x.fd, SOL_SOCKET, SO_BINDTODEVICE, dev, &len) && !strcmp(dev, ifname),
          "the exchange socket is bound to its interface");
#endif
    xchg_close(&x);
}

int main(void) {
    codec();
    parsing();
    bound(if_nametoindex("lo0") ? "lo0" : "lo");
    const char *lo = loopback();
    if (lo) admission(lo);
    else puts("test_link_xchg: no link-local loopback address; admission checks skipped");
    puts("test_link_xchg: ok");
    return 0;
}
