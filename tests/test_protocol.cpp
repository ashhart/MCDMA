#include "cx5_protocol.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace cx5;
static unsigned checks;
#define CHECK(e) do { ++checks; if (!(e)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#e); std::exit(1); } } while (0)
int main() {
    Registers r{pci_identity, 7, (35u << 16) | 16u, (5u << 16) | 8008u, 0x56, 0};
    Device d{};
    CHECK(inspect(r,d)==Error::none);
    CHECK(d.major==16 && d.minor==35 && d.patch==8008 && d.command_revision==5);
    CHECK(d.pci_revision==7 && d.log_slots==5 && d.log_stride==6);
    auto bad=r; bad.identity=0x101515b3; CHECK(inspect(bad,d)==Error::wrong_device);
    bad=r; bad.identity=pci_identity_non_ex; CHECK(inspect(bad,d)==Error::none);
    bad=r; bad.identity=UINT32_MAX; CHECK(inspect(bad,d)==Error::removed);
    bad=r; bad.initializing=0x80000000; CHECK(inspect(bad,d)==Error::initializing);
    bad=r; bad.interface_version=0x60000; CHECK(inspect(bad,d)==Error::command_revision);
    for (unsigned s=0; s<16; ++s) for (unsigned q=0; q<16; ++q) {
        bad=r; bad.queue_geometry=(q<<4)|s;
        CHECK((inspect(bad,d)==Error::none)==(q<=5 && s>=6 && s<=12 && q+s<=12));
    }
    uint8_t in[16]={0x01,0x50}, e[64], out[16];
    CHECK(encode_inline(e,in,16,16,42)==Error::none);
    CHECK(e[0]==7 && e[7]==16 && e[16]==1 && e[17]==0x50 && e[59]==16 && e[60]==42 && e[63]==1);
    uint8_t x=0; for (auto b:e) x^=b; CHECK(x==255);
    uint8_t delivery, fw;
    CHECK(decode_inline(e,42,out,16,delivery,fw)==Error::busy);
    e[63]=0; e[47]=0x7b;
    CHECK(decode_inline(e,42,out,16,delivery,fw)==Error::none && out[15]==0x7b);
    CHECK(decode_inline(e,43,out,16,delivery,fw)==Error::token_mismatch);
    e[63]=4; CHECK(decode_inline(e,42,out,16,delivery,fw)==Error::delivery && delivery==2);
    e[63]=0; e[32]=3; CHECK(decode_inline(e,42,out,16,delivery,fw)==Error::firmware && fw==3);
    e[32]=0; CHECK(decode_inline(e,42,out,8,delivery,fw)==Error::invalid_length);
    for (size_t n=0;n<34;++n) CHECK((encode_inline(e,in,n,16,42)==Error::none)==(n>=8 && n<=16));
    CHECK(encode_inline(e,in,16,16,0)==Error::invalid_token);
    CommandLease lease;
    CHECK(lease.releasable()); CHECK(lease.begin(1)==Error::none);
    CHECK(!lease.releasable()); CHECK(lease.begin(2)==Error::busy);
    CHECK(lease.finish(1)==Error::none && lease.releasable());
    CHECK(lease.begin(2)==Error::none); lease.timeout();
    CHECK(lease.poisoned() && !lease.releasable());
    CHECK(lease.begin(3)==Error::poisoned && lease.finish(2)==Error::poisoned);
    CommandLease mismatch; CHECK(mismatch.begin(9)==Error::none);
    CHECK(mismatch.finish(8)==Error::token_mismatch && mismatch.poisoned());
    std::printf("PASS %u portable checks; no hardware RDMA claimed\n",checks);
}
