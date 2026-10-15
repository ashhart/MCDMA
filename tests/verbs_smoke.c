/* Verbs smoke test against the system's real library: list the RDMA devices, then check that the exchange accepts
 * only Thunderbolt ports, using those names alone. No device is opened. */
#include "../rpc/link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void link_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  refused: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

int main(void) {
    int n = 0;
    char thunderbolt[64] = "";
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list) {
        perror("ibv_get_device_list");
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        const char *name = ibv_get_device_name(list[i]);
        printf("%s\n", name);
        if (!thunderbolt[0] && !strncmp(name, "rdma_en", 7)) snprintf(thunderbolt, sizeof(thunderbolt), "%s", name);
    }
    ibv_free_device_list(list);
    printf("verbs_smoke: %d devices\n", n);
#ifdef __APPLE__
    if (!thunderbolt[0]) return 0;
    struct ep tb, cx5;
    memset(&tb, 0, sizeof(tb));
    memset(&cx5, 0, sizeof(cx5));
    snprintf(tb.device, sizeof(tb.device), "%s", thunderbolt);
    snprintf(cx5.device, sizeof(cx5.device), "rdma_mcrdma0");
    tb.kind = LINK_TB, cx5.kind = LINK_ROCE;
    const char *port = thunderbolt + 5;
    int ok = !via_check(port, &tb) && via_check("lo0", &tb) && !via_check(port, &cx5) && via_check("lo0", &cx5);
    printf("verbs_smoke: the exchange %s Thunderbolt-only on %s\n", ok ? "is" : "is NOT", port);
    // A ConnectX link may meet elsewhere only with its peer pinned; a Thunderbolt link never may.
    int pinned_ok = !via_check("lo0/fe80::1", &cx5) && !via_check("lo0/192.0.2.1", &cx5) && via_check("lo0/fe80::1", &tb);
    printf("verbs_smoke: a pinned ConnectX peer %s meet off Thunderbolt\n", pinned_ok ? "may" : "may NOT");
    return ok && pinned_ok ? 0 : 1;
#else
    return 0;
#endif
}
