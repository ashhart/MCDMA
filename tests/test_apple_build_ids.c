#include "apple_build_ids.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *valid[] = {"26A5425a", "26A428", "26B5101f"};
    for (size_t n = 0; n < sizeof(valid) / sizeof(valid[0]); ++n) {
        const size_t bytes = strlen(valid[n]) + 1;
        const char *result = mcdma_verified_apple_build(valid[n], bytes);
        if (!result) {
            fprintf(stderr, "FAIL audited build rejected: %s\n", valid[n]);
            return 1;
        }
        assert(strcmp(result, valid[n]) == 0);
        for (size_t short_size = 0; short_size < bytes; ++short_size)
            assert(!mcdma_verified_apple_build(valid[n], short_size));
        // The helper must reject an oversized count before reading past a
        // short caller buffer, rather than passing an unchecked C string.
        assert(!mcdma_verified_apple_build(valid[n], (size_t)-1));
        char changed[32];
        memcpy(changed, valid[n], bytes);
        for (size_t i = 0; i < bytes; ++i) {
            changed[i] ^= 1;
            assert(!mcdma_verified_apple_build(changed, bytes));
            changed[i] ^= 1;
        }
        changed[bytes] = 'x';
        assert(!mcdma_verified_apple_build(changed, bytes + 1));
    }
    assert(!mcdma_verified_apple_build(NULL, 0));
    assert(!mcdma_verified_apple_build(NULL, (size_t)-1));
    assert(!mcdma_verified_apple_build("26A429", sizeof("26A429")));
    assert(!mcdma_verified_apple_build("26A428a", sizeof("26A428a")));
    assert(!mcdma_verified_apple_build("26A428\0x", sizeof("26A428\0x")));
    assert(!mcdma_verified_apple_build("26B5101", sizeof("26B5101")));
    assert(!mcdma_verified_apple_build("26B5101g", sizeof("26B5101g")));
    puts("PASS exact audited beta/release build IDs, malformed lengths, terminators and near-miss rejection");
    return 0;
}
