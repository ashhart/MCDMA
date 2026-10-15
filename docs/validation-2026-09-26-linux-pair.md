# Linux pair validation, 26 September 2026

`benchmarks/mcdma_bw.c` and `benchmarks/run_bw.py --mac-platform linux` were run between two Linux hosts, neither of them a Mac: an AMD Strix Halo machine driving a ConnectX-5 Ex through an OWC Helios 5S over USB4, and a GB10 Spark-class machine with a ConnectX-7. The MCDMA kernel extension and provider are macOS components and were not involved; both ends used stock rdma-core. The result is a correctness and bandwidth baseline for the benchmark tooling on a Linux pair, and a measurement of what the USB4 host path delivers.

## Test setup and measurement boundary

| | Host A (initiator for the `mac` rows) | Host B (initiator for the `peer` rows) |
|---|---|---|
| Machine | AMD Ryzen AI Max (Strix Halo) mini PC | ASUS Ascent GX10 (GB10) |
| NIC path | ConnectX-5 Ex MCX516A-CDAT in an OWC Helios 5S, USB4 to the host | ConnectX-7, internal |
| RDMA device / netdev | `rocep3s0f0` / `enp3s0f0np0` | `rocep1s0f0` / `enp1s0f0np0` |
| OS | Linux, rdma-core (versions not recorded in this run) | Ubuntu 24.04.5 LTS, kernel `7.0.0-1019-nvidia`, rdma-core `50.0-2ubuntu0.2`, CX-7 firmware `28.45.4028` |
| RoCE v2 GID | index 3, IPv4-mapped (`::ffff:` + port address) | index 5, IPv4-mapped |

The two NICs were cabled directly with one 0.5 m 200G QSFP56 passive copper cable; the link negotiated 100 Gb/s (ethtool on host B: `Speed: 100000Mb/s`). Both ports carried IPv4 addresses on one private /24. No switch.

The benchmark binary on each host was built from `benchmarks/mcdma_bw.c` at the commit under test with `gcc -O2 mcdma_bw.c -libverbs`. Hashes recorded by the runner:

| Host | `mcdma-bw` SHA-256 |
|---|---|
| A | `2da764b7d770cf663b42156af0fdb59bf06d8f8b09977697df0bfbd74dcce09e` |
| B | `373d65568470c47e9d4c57f26feb6819c023e3e26aacd4bf840e543b50b0a321` |

The two hashes differ because the hosts differ in CPU architecture; both come from the same source.

Invocation (from a third machine, which only drove SSH):

```bash
python3 benchmarks/run_bw.py --mac-platform linux \
  --mac-host "$HOST_A" --peer-host "$HOST_B" \
  --mac-bw ~/mcdma-bw --peer-bw ~/mcdma-bw \
  --mac-interface enp3s0f0np0 --peer-interface enp1s0f0np0 \
  --mac-device rocep3s0f0 --peer-device rocep1s0f0 \
  --mac-gid-index 3 --peer-gid-index 5 \
  --ops write,read --sizes 1048576,4194304 --depths 1,7 \
  --mtu 1024 --total 8589934592 --output "$OUT"
```

Defaults otherwise: one RC QP, shared CQ, three rounds, one unmeasured warmup before each measured trial, 8 GiB per trial, 4 MiB verification budget, finish marker chosen by the tool (`imm`). Throughput is payload bytes over the initiator's timed data loop, from the first post to the receiver-visible finish marker, as elsewhere in this repository. Only initiator rows with `warmup=0` are counted.

Preflight passed on both hosts: each GID index was RoCE v2 on the named netdev, both binaries were hashed, and before each pair each host's IPv4 neighbour table held the other port's address with a link-layer address (states seen: REACHABLE, STALE, DELAY).

## Results

48 measured trials (16 configurations x 3, each after one unmeasured warmup), 0 failed, 0 completion errors, 0 mismatches, `guard_ok=1` on every row. Medians of three, Gbit/s (`benchmarks/bw_summary.py` over the retained CSVs):

| Initiator | Op | Request | Depth (effective) | Median | Min | Max | Verified bytes / trial |
|---|---|---:|---:|---:|---:|---:|---:|
| A | WRITE | 1 MiB | 1 (1) | 29.277 | 29.247 | 29.329 | 12288 |
| A | WRITE | 1 MiB | 7 (7) | 29.753 | 29.726 | 29.869 | 86016 |
| A | WRITE | 4 MiB | 1 (1) | 29.644 | 29.610 | 29.774 | 12288 |
| A | WRITE | 4 MiB | 7 (1) | 29.666 | 29.631 | 29.773 | 12288 |
| A | READ | 1 MiB | 1 (1) | 30.059 | 30.045 | 30.074 | 12288 |
| A | READ | 1 MiB | 7 (7) | 30.517 | 30.517 | 30.518 | 86016 |
| A | READ | 4 MiB | 1 (1) | 30.397 | 30.370 | 30.398 | 12288 |
| A | READ | 4 MiB | 7 (1) | 30.400 | 30.390 | 30.404 | 12288 |
| B | WRITE | 1 MiB | 1 (1) | 30.518 | 30.512 | 30.520 | 12288 |
| B | WRITE | 1 MiB | 7 (7) | 30.520 | 30.520 | 30.520 | 86016 |
| B | WRITE | 4 MiB | 1 (1) | 30.520 | 30.519 | 30.524 | 12288 |
| B | WRITE | 4 MiB | 7 (1) | 30.518 | 30.515 | 30.518 | 12288 |
| B | READ | 1 MiB | 1 (1) | 29.494 | 29.401 | 29.498 | 12288 |
| B | READ | 1 MiB | 7 (7) | 29.768 | 29.747 | 29.780 | 86016 |
| B | READ | 4 MiB | 1 (1) | 29.624 | 29.610 | 29.659 | 12288 |
| B | READ | 4 MiB | 7 (1) | 29.647 | 29.636 | 29.651 | 12288 |

By payload direction: host B to host A (A READ, B WRITE) ran at 30.1 to 30.5 Gbit/s; host A to host B (A WRITE, B READ) at 29.3 to 29.8. Every configuration's median lies between 29.3 and 30.5 Gbit/s, independent of request size and depth. An earlier run the same day, with the endpoint handshake relayed by hand instead of by the runner (RoCE v1, GID index 0, 1 MiB, depth 7, A WRITE), measured 29.75, 29.72 and 29.75 Gbit/s, in line with the table.

Both sides polled at 100% CPU (`cpu_pct`), as the tool busy-polls completions. Verification is sampled, as in the other reports: the verified column is the number of bytes the receiver checked against the seeded pattern per trial, not the payload size.

## Observations

- **A flat ~30 Gbit/s in both directions.** The link negotiated 100 Gb/s and host B's ConnectX-7 is internal, so the ceiling is on host A's side. The Helios enclosure reaches host A over USB4, and a PCIe tunnel on that path is the likely limit. We did not measure the tunnel separately, so this is an attribution, not a measurement. For comparison, the 17 September report measured a Mac Studio and a DGX Spark at about 50.5 Gbit/s into the Studio and 29.4 out; on this Linux host the into-host direction is held at about 30 as well.
- **The depth 7 clamp.** Four requested 4 MiB configurations at depth 7 ran at effective depth 1 because host A's locked-memory limit (8 MiB) capped the registered region. At 1 MiB, depth 7 fit. Raising `ulimit -l` on host A is the fix; it did not change the throughput here because the path was already saturated at depth 1.
- **RoCE v2 over link-local GIDs did not connect on this pair.** A separate manual check with the MAC-derived link-local RoCE v2 GIDs failed at `modify QP to RTR`; the IPv4-mapped RoCE v2 GIDs above connected at once. The runner accepts IPv4-mapped GIDs only with `--mac-platform linux` and checks the IPv4 neighbour table for them; the macOS path still requires the MAC-derived link-local GID and static IPv6 neighbours.
- **Host B's netdev MTU was raised from 1500 to 9000 around the start of the sweep.** The RDMA path MTU was fixed at 1024 by `--mtu` throughout, and no step is visible in the rows.

## Not covered

READ/WRITE with more than one QP, per-QP CQs, SEND, payload (file) mode, `mcdma-rpcd` between two Linux hosts, and any model integration. Host A's kernel and rdma-core versions were not captured by this run.
