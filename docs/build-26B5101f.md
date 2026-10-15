# macOS 27.2 beta, build 26B5101f: interface comparison against 26A428

Build `26B5101f` (macOS 27.2 beta, Darwin 27.2.0, `xnu-13432.40.177.0.3~56`) was compared
with the validated build `26A428` for every Apple interface the native driver and the verbs
provider use. No change to those interfaces was found, so `26B5101f` joins the build
allowlist. The four-way byte-verifying transfer check then passed on 26B5101f hardware in all
three posting modes (see [Hardware check](#hardware-check)).

## How it was compared

`tools/compare-apple-build.py` does the whole comparison and can be rerun for a later build:

```sh
python3 tools/build.py native
python3 tools/compare-apple-build.py \
  --old https://updates.cdn-apple.com/2026FallFCS/afcfc88e-bbe6-44bf-a5da-07c56eebc06c/UniversalMac_27.0_26A428_Restore.ipsw \
  --new /System/Volumes/Preboot/<volume>/boot/<hash>/System/Library/Caches/com.apple.kernelcaches/kernelcache \
  --kext build/MCDMACX5Native.kext --out results/abi-26B5101f --userspace
```

For the IPSW it fetches only `kernelcache.release.mac15j` (about 33 MB) with HTTP range
requests. It decodes both kernelcaches with `libcompression` and extracts the kernel,
`IORDMAFamily` and `IOPCIFamily` fileset entries. Then it compares them with Xcode's
`llvm-nm` and `llvm-objdump`. The `--new` kernelcache was the boot kernelcache of a running
26B5101f Mac Studio M3 Ultra, whose `IORDMAFamily` load address matched the extracted entry.
Kernel collections are not committed (see CONTRIBUTING.md).

## Results

| Check | 26A428 | 26B5101f |
|---|---|---|
| Kext imports resolved (kernel, IORDMAFamily, IOPCIFamily exports) | 409 of 409 | 409 of 409 |
| Exports removed / added | — | 0 / 5 (`ib_map_mr_sg` and four unrelated kernel symbols) |
| Exported methods of the 26 IOKit/libkern classes the kext names | identical | identical |
| `_ib_alloc_device` size check (`sizeof(ib_device)`) | `> 0x9a7` | `> 0x9a7` |
| Device-op calls as (pointer offset, PAC discriminator) pairs | 167 | 167, same pairs |
| `libibverbs` private symbols the provider calls | — | all 21 exported |
| Provider static-assert offsets used by `libibverbs` (`0x14`, `0x28`, `0x38`, `0x298`, `0x2b8`, `0x140`, MR type `0x30`) | — | all found at the asserted offsets |

Of 1,227 `IORDMAFamily` functions, 60 differ once relocated addresses are masked. 35 of them
differ only in `__LINE__` constants passed to assertions and logging, including
`ib_set_device_ops`, `ib_uverbs_poll_cq` and `IORDMAInterface::quiesce`. The others were read
one by one:

- **Address resolution was reworked.** `addr_resolve`, `rdma_resolve_ip`,
  `rdma_addr_find_l2_eth_by_grh`, `get_mac_ipv4`/`get_mac_ipv6` and the request timers changed.
  Resolution is now synchronous, through the new `addr_resolve_sync`. IPv6 neighbours are
  still read with `nd6_lookup_ipv6`, and the MAC still comes from `sdl_data + sdl_nlen`; the
  new `mac_from_lladdr` also rejects link-layer addresses that are not 6 bytes long. The static
  IPv6 neighbour entries in install.md resolve as before.
- **`__ib_modify_qp`.** For userspace callers (`udata` present, the provider's path), the
  destination MAC is still always resolved with `ib_resolve_eth_dmac`. Kernel callers that
  supply a non-zero MAC now keep it, where 26A428 never resolved one for them.
- **`IORDMAInterface::registerIBInterface`.** It now also stores the device's `IOMapper` at
  `ib_device+0x0`, beside the existing `+0x908`/`+0x910` stores. The driver neither reads nor
  writes that field: its `+0x0` reads are the device pointer of Apple's PD/CQ/QP objects. The
  driver's owner pointer at `+0x9a8` still sits just past `sizeof(ib_device)`.
- **`ib_device_ndev_cb_enter`/`_exit`.** The netdev callbacks now hold the
  `ib_device` reference (`+0x880`, released with `complete(+0x884)`, as `ib_device_put` already
  did in 26A428) instead of the embedded device's reference at `+0x5c8`. These fields existed
  in both builds, and the get/put pair matches.
- **`alloc_uobj`/`uobject_free`.** The allocation size is now stored in padding at `+0x3c`.
  Every existing uobject field keeps its offset (`+0x8`, `+0x20`, `+0x34`, `+0x38`, `+0x50`).
- **`darwin_dma_alloc`/`darwin_dma_free`** (MAD buffers) and
  **`IORDMAFamilyUC::BindInterface`** (now terminates the interface name at 16 bytes) are not
  on the driver's path.
- **`IORDMAInterface::start`, `rdma_restrack_add`, `rdma_resolve_addr`,
  `roce_resolve_route_from_path`.** Only relocated globals differ (offsets move by a common
  delta through `adrp` bases).

## Hardware check

On 2026-10-09: a Mac Studio M3 Ultra (256 GB) on 26B5101f, OWC Helios 5S over Thunderbolt 5,
and a ConnectX-5 Ex (`15b3:1019`). The peer was a DGX Spark-class GB10 (ConnectX-7, firmware
28.45.4028, Ubuntu 24.04). Driver 0.1.18 was built from this branch, ad-hoc signed with the
three personality flags. The kext loaded with the candidate's UUID and reported
`MCDMANativeBuild = "26B5101f"`. Ethernet MTU 9000, RDMA path MTU 4096, payload 4096 bytes.
The link negotiated 40GBASE-CR4 (QSFP+ cable).

`cx5-native-check --require-gid` passed (one active CX5 port, MAC-derived link-local RoCE v2
GID, `errors=0`). `tools/native_cross_host.py` then passed in each mode, with every operation
verifying 4096 bytes:

| Mode (`--mac-cq-map/--mac-user-post/--mac-user-bf`) | Mac WRITE | Mac READ | Spark WRITE | Spark READ | Markers confirmed |
|---|---|---|---|---|---|
| kernel (0/0/0) | pass | pass | pass | pass | — |
| direct (2/1/0) | pass | pass | pass | pass | CQ mapping, user post |
| BlueFlame-64 (2/1/64) | pass | pass | pass | pass | CQ mapping, user post, BF64 |

Afterwards the driver showed no quarantine, no outbound PCIe stalls and no CRC errors. Its
PCIe TX error count was 2 on each function, the same as before any traffic. These are
correctness results only; no latency or bandwidth was measured on this build.

## Not covered

Kernel behaviour outside these binaries (scheduler, VM, PCIe/Thunderbolt) was not compared.
Neither was `AppleEthernetMLX5`, which the driver does not call. Because 26B5101f is a beta, the
release 27.2 build will have a different ID and needs this comparison again.
