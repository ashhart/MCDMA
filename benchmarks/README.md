# Real-payload correctness check

`run_bw.py --payload /source/file --dump /new/destination` transfers one
resident file through the registered buffers. Select one `--ops write` or
`--ops read`, one size, one depth, `--qps 1`, one initiator, one CQ mode,
`--repeats 1` and `--warmup 0`. Both clients must be updated. Paths are on
the respective data-source and receiver hosts; for READ the responder is
the source and the initiator receives the dump.

The nonempty regular file must fit `bytes * agreed_depth` after resource
clamping; oversized inputs fail instead of wrapping or truncating. The file
length replaces `--total`. The final request is zero-padded and the dump
excludes padding. Each slot is transferred once; SEND and multi-QP file
layouts are rejected. This is not an arbitrary-size streaming handoff.

The receiver checks the entire file with CRC-64/ECMA and validates DMA
guards before exclusively creating the destination; existing files and
symlinks are not overwritten. CRC64 detects accidental corruption, not
malicious modification. A checksum failure counts as one mismatch and
prevents a dump. File I/O and checksums are outside the RDMA timer; rows
are labelled `measurement=resident-payload` and rejected by the sustained
bandwidth summariser. Use seeded mode for sustained-bandwidth comparisons.

# Two Linux hosts

`--mac-platform linux` lets `run_bw.py` drive two Linux RDMA hosts on stock
rdma-core. The `--mac-*` flags then name the first Linux host. Compile
`mcdma_bw.c` on both (`gcc -O2 mcdma_bw.c -libverbs -o mcdma-bw`) and run:

```sh
python3 benchmarks/run_bw.py --mac-platform linux \
  --mac-host "$A_SSH" --peer-host "$B_SSH" \
  --mac-bw "$A_BW" --peer-bw "$B_BW" \
  --mac-interface "$A_IF" --peer-interface "$B_IF" \
  --mac-device "$A_RDMA_DEVICE" --peer-device "$B_RDMA_DEVICE" \
  --mac-gid-index "$A_GID_INDEX" --peer-gid-index "$B_GID_INDEX" \
  --ops write --sizes 4194304 --depths 1 --qps 1 --total 8589934592 \
  --mtu 4096 --finish flag --verify-bytes 1048576 --timeout 120 \
  --output results/linux-pair --dry-run
```

Drop `--dry-run` to run it. `--mac-provider`, `--mac-checker` and the
`--mac-cq-map`, `--mac-user-post` and `--mac-user-bf` modes do not apply and
are rejected. The command runs with no `env IBV_DRIVERS` or `MCDMA_*` prefix,
and both `--mac-gid-index` and `--peer-gid-index` are required. Preflight gives both hosts the same
check: the GID index must be RoCE v2 on the named interface, and binaries are
hashed with `sha256sum`. The manifest records `mac_platform`, and a
`MCDMA_*` provider marker on the first host fails the run.

The tested Linux path is RoCE v2 over IPv4 addresses. Give each RDMA port an
IPv4 address on a shared subnet and pass, for each host, the index of the
RoCE v2 GID that reads `::ffff:<that port's IPv4 address>` (see
`/sys/class/infiniband/<device>/ports/1/gids` and `gid_attrs/types`). Both
hosts must then hold an ARP entry with a link-layer address for the other
(`ip -4 neigh show to <address> dev <interface>`; REACHABLE, STALE, DELAY,
PROBE or PERMANENT). Ping the other port once if the entry is missing. On
the one Linux pair tested so far, RoCE v2 over the MAC-derived link-local
GID failed at "modify QP to RTR". Link-local GIDs are still accepted, with
the static IPv6 neighbour check (`ip -6 neigh`), but have not connected on
that pair. Both hosts must use the same GID kind.
