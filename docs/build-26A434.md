# macOS 27.0.1, build 26A434: interface comparison against 26A428

Build `26A434` (macOS 27.0.1, Darwin 27.0.0, `xnu-13432.1.9~1`) was compared with the validated build
`26A428` for the Apple interfaces `tools/compare-apple-build.py` inspects. No difference was found, so
`26A434` joins the build allowlist. The four-way byte-verifying transfer check then passed on 26A434
hardware in all three posting modes, on a tree that also carried other open pull requests (see
[Hardware check](#hardware-check)).

## Why

On 26A434 an installed 0.1.18 kext loads but does not attach: `supported_build()` is false, so
`Transport::attach()` returns `kIOReturnUnsupported` (`native/kernel_transport.cpp`) and no
`MCDMACX5Native` instance appears. On the Mac described below, which runs a separately patched build for a
ConnectX-5 `15b3:1017` (see [Hardware check](#hardware-check)), Apple's `AppleEthernetMLX5` then took both
functions of the card, aborted in `AppleEthernetMLX5CmdWorkEnt::waitFor` during `startEQs()`, and the
kernel panicked after the dext crashed too many times. That is an observation on that configuration. This
change only adds the build ID; on its own it does not support a `1017` card.

## How it was compared

```sh
python3 tools/build.py native
python3 tools/compare-apple-build.py \
  --old https://updates.cdn-apple.com/2026FallFCS/afcfc88e-bbe6-44bf-a5da-07c56eebc06c/UniversalMac_27.0_26A428_Restore.ipsw \
  --new /System/Volumes/Preboot/<volume>/boot/<hash>/System/Library/Caches/com.apple.kernelcaches/kernelcache \
  --kext build/MCDMACX5Native.kext --out results/abi-26A434 --userspace
```

`--new` was the boot kernelcache of a running 26A434 Mac Studio M3 Ultra.

## Results

| Check | 26A428 | 26A434 |
|---|---|---|
| Kext imports resolved (kernel, IORDMAFamily, IOPCIFamily exports) | 409 of 409 | 409 of 409 |
| Exports removed / added | n/a | 0 / 0 |
| Exported methods of the 26 IOKit/libkern classes the kext names | identical | identical |
| `_ib_alloc_device` size check (`sizeof(ib_device)`) | `> 0x9a7` | `> 0x9a7` |
| Device-op call sites (pointer offset, PAC discriminator) | 167 | 167, same pairs |
| `IORDMAFamily` functions that differ once relocated addresses are masked | n/a | 0 of 1,227 |
| `libibverbs` private symbols the provider calls (running host) | n/a | all exported |
| Provider static-assert offsets found in `libibverbs` (running host) | n/a | all seven |

The kernel, `IORDMAFamily` and `IOPCIFamily` entries extracted from the two kernelcaches carry the same
`LC_UUID`, and the running 26A434 system reports the same `IORDMAFamily` and `IOPCIFamily` UUIDs through
`kmutil showloaded`:

| Component | `LC_UUID` on 26A428 and 26A434 |
|---|---|
| `com.apple.kernel` | `4F99A015-04DC-33E2-AED7-D999C931A6EC` |
| `com.apple.iokit.IORDMAFamily` | `A266614B-439F-3B38-9862-F6D245F8327D` |
| `com.apple.iokit.IOPCIFamily` | `FDE8480B-8E13-3597-8B01-8DCBDBC92F62` |

Matching UUIDs, unchanged exports and identical normalized `IORDMAFamily` functions support
compatibility; no byte-for-byte comparison of the kernel or `IOPCIFamily` was made. The kernel collection's
fileset UUID differs between the builds; which members changed was not examined. The tool's
"only old/only new" lines name the two extracted `.macho` files; that is its function parser picking up
the file header, not a function.

## Hardware check

The four-way byte-verifying check (`tools/native_cross_host.py`, path MTU 4096, 4096-byte payloads)
passed on 26A434 in all three posting modes: kernel (`--mac-cq-map 0 --mac-user-post 0 --mac-user-bf 0`),
direct (`--mac-cq-map 2 --mac-user-post 1 --mac-user-bf 0`) and BlueFlame-64 (`--mac-user-bf 64`). Mac
WRITE/READ and Linux-peer WRITE/READ each verified 4096 bytes. User posting was confirmed in both direct
modes and BlueFlame-64 in the last. No quarantine, start error or new `AppleEthernetMLX5` crash report
followed. These are correctness results only; nothing was benchmarked.

The tree under test was **not** this pull request's tree. The only card available is a ConnectX-5
`15b3:1017`, which needs #11 and #13:

| Part | Revision |
|---|---|
| Mac source | `e12c639` (head of #11) + `437e579` (same content as #13 head `d567fd8`) + `87d967b` (adds `26A434` to the same allowlist lines as this change; local, not published); no uncommitted modifications |
| Loaded kext | 0.1.18, UUID `848F1CCE-FA23-397B-8443-EA816E3212D1`, built from that tree; candidate and `kmutil showloaded` agree; registry `MCDMANativeBuild = "26A434"` (the OS build it accepted) |
| Mac provider | `libmcdma-rdmav34.so`, SHA-256 `6dc5361ffb4cc96efa63c17caaedffde160bb9dc787980ad8731546532916f9e`, same build; installed copy identical |
| Linux peer client | `build/verbs-peer` built from `e12c639` |

Setup: Mac Studio M3 Ultra on 26A434, OWC Helios 5S, ConnectX-5 `15b3:1017`, one Linux peer with a
ConnectX-7 through a switch port fixed to 40GBASE-CR4. The offline tests (`python3 tools/build.py test`),
the native build and the CLI tests ran on this pull request's tree on the same 26A434 Mac.

## Not covered

- The unmodified tree of this pull request was not hardware-tested, and no ConnectX-5 Ex was available.
- `--userspace` checks 21 `libibverbs` exports and seven offsets on the running host; it does not compare
  the userspace implementation or its dependencies between builds.
- Components the driver does not link against directly were not compared: other kernel collection
  members, Thunderbolt/PCIe and mapper implementations, driver matching, DriverKit.
- The installation route in [install.md](install.md) was not repeated on 26A434. The kext was replaced on
  an existing installation with the enclosure connected and powered, after Apple's driver had attached to
  the card and crashed on it repeatedly; a normal restart was enough for MCDMA to take both functions. That
  is one machine and one boot, not an installation recipe.
- No lifecycle, unload, sustained-load or performance testing.
