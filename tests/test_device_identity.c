#include "cx5_device.h"
#include <assert.h>

int main(void) {
    assert(mcdma_supported_device(0x15b3,0x1015));
    assert(mcdma_supported_device(0x15b3,0x1019));
    assert(mcdma_supported_device(0x15b3,0x1017));
    assert(!mcdma_supported_device(0x15b3,0x101f));
    assert(!mcdma_supported_device(0xffff,0x1015));
    assert(!mcdma_supported_device(0,0));
    assert(mcdma_native_peer(0x15b3,0x1015,2));
    assert(mcdma_native_peer(0x15b3,0x1019,2));
    assert(mcdma_native_peer(0x15b3,0x1017,2));
    assert(!mcdma_native_peer(0x15b3,0x1015,100));
    assert(!mcdma_native_peer(0,0,100));
    assert(!mcdma_native_peer(0,0,2));
    return 0;
}
