/* Offline test of mcdma-rpcd's link labels: MCDMA_RPCD_HOSTS="NAME=HOST[,NAME=HOST]" names the peer STATUS reports
 * as host=; a malformed list is refused whole, and a link without a label prints no host= at all. */
#include "../rpc/rpcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "test_rpcd_hosts: %s\n", what);
        exit(1);
    }
}

static int host_of(const char *env, const char *name, char *out, size_t n) {
    if (env) setenv("MCDMA_RPCD_HOSTS", env, 1);
    else unsetenv("MCDMA_RPCD_HOSTS");
    return link_host(name, out, n);
}

int main(void) {
    char host[64], field[80];
    check(host_of(NULL, "worker-a", host, sizeof(host)) == 0 && !*host, "no variable: no label");
    check(host_of("", "worker-a", host, sizeof(host)) == 0 && !*host, "an empty variable: no label");
    check(host_of("worker-a=spark-a", "worker-a", host, sizeof(host)) == 1 && !strcmp(host, "spark-a"),
          "one entry names its link");
    check(host_of("worker-a=spark-a", "worker-b", host, sizeof(host)) == 0 && !*host,
          "a link the list does not name has no label");
    check(host_of("worker-a=spark-a,worker-b=192.0.2.7", "worker-b", host, sizeof(host)) == 1 &&
              !strcmp(host, "192.0.2.7"),
          "the second of two entries");
    check(host_of("kv-a=fe80::1%en0,worker-a=spark-a.example", "worker-a", host, sizeof(host)) == 1 &&
              !strcmp(host, "spark-a.example"),
          "a host may carry any printable characters but whitespace and commas");
    check(host_of("worker-a=spark-a,worker-a=spark-b", "worker-a", host, sizeof(host)) == 1 &&
              !strcmp(host, "spark-a"),
          "the first entry for a name wins");
    /* every malformed list is refused for every link, so a typo cannot silently drop one label */
    const char *bad[] = {"worker-a", "worker-a=", "=spark-a", "worker-a=spark a", "worker-a=spark-a,",
                         ",worker-a=spark-a", "worker-a=spark-a,,worker-b=x", "bad name=spark-a",
                         "worker-a=spark-a\tb", "a-name-that-is-far-too-long=spark-a",
                         "worker-a=0123456789012345678901234567890123456789012345678901234567890123"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char what[160];
        snprintf(what, sizeof(what), "a malformed list is refused: %s", bad[i]);
        check(host_of(bad[i], "worker-a", host, sizeof(host)) < 0, what);
        check(host_of(bad[i], "other", host, sizeof(host)) < 0, what);
    }
    check(host_of("worker-a=012345678901234567890123456789012345678901234567890123456789012", "worker-a", host,
                  sizeof(host)) == 1 && strlen(host) == 63,
          "a 63-character host is the longest");
    check(host_of("worker-a=spark-a", "worker-a", NULL, 0) == 1, "a caller may only ask whether a label exists");
    setenv("MCDMA_RPCD_HOSTS", "worker-a=spark-a", 1);
    host_field("worker-a", field, sizeof(field));
    check(!strcmp(field, " host=spark-a"), "the STATUS field for a labelled link");
    host_field("worker-b", field, sizeof(field));
    check(!*field, "the STATUS field for an unlabelled link is empty");
    puts("test_rpcd_hosts: ok");
    return 0;
}
