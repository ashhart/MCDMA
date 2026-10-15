/* mcdma-rpcd: request/reply transport between applications on two machines over MCDMA RDMA.
 *
 * Applications never open a verbs context. Each talks to its local daemon through a shared-memory mailbox, and
 * only the daemons hold queue pairs, so an application that crashes or is killed cannot leave a QP behind. One
 * source builds on Linux (libibverbs) and macOS (Apple's librdma, with MCDMA's CX5 provider or Thunderbolt RDMA).
 *
 *   mcdma-rpcd listen [--owner USER] NAME DEVICE GID_INDEX PATH_MTU VIA:PORT [REQ_MIB REP_MIB]
 *       One end of one link. Mailbox /dev/shm/mcdma-rpc.NAME on Linux, POSIX shared memory /mcdma-rpc.NAME on a
 *       Mac. The setup exchange listens on UDP PORT of the Thunderbolt IP interface VIA and admits only link-local
 *       datagrams that arrive there with hop limit 255, so only the machine at the other end of that cable gets in;
 *       VIA = IFACE/fe80::ADDR admits only that address. The Unix socket /tmp/mcdma-rpcd.NAME.sock takes one service
 *       ("MODE poll") and answers STATUS and SHUTDOWN. Started as root on Linux, --owner gives the mailbox and socket
 *       to USER, whose services then attach without root.
 *   mcdma-rpcd connect PEER...
 *       PEER = name,via,port,device,gid_index,path_mtu[,req_mib,rep_mib]
 *       e.g. worker-a,en2,18620,rdma_en2,1,4096,4,64
 *       One POSIX shared memory mailbox /mcdma-rpc.NAME and one thread per peer; each finds its listen end by
 *       multicast on VIA (or at VIA's pinned address) and offers from an ephemeral port to PORT. The Unix socket
 *       /tmp/mcdma-rpcd.sock answers STATUS and SHUTDOWN (tears every verbs object down, then exits).
 *   mcdma-rpcd version
 *
 * There is no TCP anywhere: the exchange is a few hundred bytes of IPv6 link-local datagrams on the Thunderbolt
 * cable, and every payload byte moves by RDMA. MCDMA_RPCD_SOCKET replaces the Unix socket path. Mailboxes and
 * sockets are owner-only: run the daemon as the user whose applications use it. NAME is 1-20 characters of
 * [A-Za-z0-9_-]. docs/link-daemon.md describes the mailbox (protocol 1), the exchange and both link kinds.
 *
 * Safety. The connect end takes its queue pairs past INIT only after the listen end has answered from RTS, posts
 * nothing before its own RTS, gives up on a peer by destroying its queue pairs inside a live process (never by
 * exiting), waits out or flushes every posted work request before destroying anything, and tears every object down
 * on every exit path. Every wait is bounded, so shutdown never hangs on a silent peer. One daemon serves each link
 * name: a lock file in /tmp is taken before any device or mailbox is touched, and a second daemon refuses to start.
 * SHUTDOWN, SIGINT and SIGTERM all stop it the same orderly way; SIGHUP is ignored, and SIGKILL, which skips the
 * teardown, must never be used.
 */
#include "rpcd.h"

#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VIA:PORT, where VIA is IFACE or IFACE/fe80::ADDR; the port follows the last colon. */
static int parse_via(const char *text, char *via, size_t n, int *port) {
    const char *colon = strrchr(text, ':');
    char ifname[32];
    struct in6_addr pin;
    int pinned = 0;
    if (!colon || colon == text || (size_t)(colon - text) >= n) return -1;
    memcpy(via, text, (size_t)(colon - text));
    via[colon - text] = 0;
    char *end = NULL;
    long value = strtol(colon + 1, &end, 10);
    if (!colon[1] || *end || value <= 0 || value > 65535) return -1;
    *port = (int)value;
    return via_parse(via, ifname, sizeof(ifname), &pin, &pinned);
}

static int usage(void) {
    fprintf(stderr, "usage: mcdma-rpcd listen [--owner USER] NAME DEVICE GID_INDEX PATH_MTU VIA:PORT [REQ_MIB REP_MIB]\n"
                    "       mcdma-rpcd connect name,via,port,device,gid_index,path_mtu[,req_mib,rep_mib] ...\n"
                    "       mcdma-rpcd version\n"
                    "VIA is a Thunderbolt IP interface, optionally /fe80::ADDR of the one peer to admit.\n"
                    "A ConnectX link may also meet on another shared link, such as the LAN, with that peer pinned.\n");
    return 2;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* a closed terminal or SSH session must not stop a daemon that holds queue pairs */
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "version")) {
        printf("mcdma-rpcd %s protocol %d\n", RELEASE, PROTOCOL);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "listen")) {
        struct owner owner = {0};
        char **arg = argv + 2;
        int left = argc - 2;
        if (left >= 2 && !strcmp(arg[0], "--owner")) {
#ifndef MCDMA_RPC_BOX_DIR
            fprintf(stderr, "--owner is for Linux listen ends; run the daemon as the mailbox's user\n");
            return 2;
#endif
            struct passwd *user = getpwnam(arg[1]);
            if (!user) {
                fprintf(stderr, "unknown user %s\n", arg[1]);
                return 2;
            }
            owner = (struct owner){1, user->pw_uid, user->pw_gid};
            arg += 2;
            left -= 2;
        }
        if (left != 5 && left != 7) return usage();
        uint64_t req = 4 << 20, rep = 4 << 20;
        if (!valid_name(arg[0])) {
            fprintf(stderr, "NAME must be 1-20 characters of [A-Za-z0-9_-]\n");
            return 2;
        }
        if (left == 7 && (parse_mib((unsigned)strtoul(arg[5], NULL, 10), &req) ||
                          parse_mib((unsigned)strtoul(arg[6], NULL, 10), &rep))) {
            fprintf(stderr, "mailbox halves must be multiples of 4 MiB up to %d MiB\n", 4 * MAX_SEGS);
            return 2;
        }
        char via[96];
        int port = 0;
        if (parse_via(arg[4], via, sizeof(via), &port)) return usage();
        return run_listen(arg[0], arg[1], atoi(arg[2]), atoi(arg[3]), via, port, req, rep, &owner);
    }
    if (argc >= 3 && !strcmp(argv[1], "connect")) {
        const char *pull = getenv("MCDMA_RPC_PULL");
        return run_connect(argc - 2, argv + 2, !(pull && !strcmp(pull, "1")));
    }
    return usage();
}
