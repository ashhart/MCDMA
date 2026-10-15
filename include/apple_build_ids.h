#pragma once
#include <stddef.h>

// Shared kernel/userspace gate for the Apple interfaces we have inspected.
// The byte count includes the terminal NUL returned by kern.osversion.
// Return a known constant only after a full, bounded match, never a prefix.
static inline const char *mcdma_verified_apple_build(const char *build, size_t bytes) {
    // 26A428 was compared against the beta kernel and userspace contracts;
    // see the private build-26A428-audit evidence retained with this work.
    // 26B5101f (27.2 beta) was compared against 26A428; see
    // docs/build-26B5101f.md and tools/compare-apple-build.py.
    static const char *const verified[] = {"26A5425a", "26A428", "26B5101f"};
    if (!build) return NULL;
    for (size_t entry = 0; entry < sizeof(verified) / sizeof(verified[0]); ++entry) {
        const char *known = verified[entry];
        size_t length = 0;
        while (known[length]) ++length;
        if (bytes != length + 1) continue;
        size_t i = 0;
        while (i < bytes && build[i] == known[i]) ++i;
        if (i == bytes) return known;
    }
    return NULL;
}
