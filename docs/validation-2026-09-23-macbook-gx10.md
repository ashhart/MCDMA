# 0.1.18 on a MacBook Pro and an ASUS GX10, 23 September 2026

Contributed report, not a maintainer measurement. It repeats the correctness checks from the [installation guide](install.md#5-verify-rdma-before-using-it), the README latency command, and the bandwidth recipe from the [17 September report](validation-2026-09-17.md#reproducing-the-bandwidth-shape) on a different Mac and a different GB10 peer. The transport tests below (correctness, latency, bandwidth) were measured on one link, on one boot of each machine, on 23 September 2026 between 19:03 and 19:24 UTC; the Mac booted at 18:59:06 UTC. The model runs were measured later on the same link, with their own times given in each section.

## Test setup

| | Mac side | Peer side |
|---|---|---|
| Machine | MacBook Pro, Apple M5 Max, 128 GB (`Mac17,6`) | ASUS Ascent GX10 (GB10) |
| OS | macOS 27.0 build `26A428`, SIP disabled, Reduced Security, `rdma_ctl` enabled | Ubuntu 24.04.5 LTS, kernel `7.0.0-1019-nvidia` |
| NIC | ConnectX-5 Ex `15b3:1019` in an OWC Mercury Helios 5S | ConnectX-7 (`vendor_part_id` 4129), firmware `28.45.4028` |
| Host path | PCIe Gen4 x4, 16 GT/s (System Information); the driver's `MCDMAPCIePath` readout in `ioreg` shows max payload 128 bytes and max read request 512 bytes at the card | |
| Interface | `mcrdma3` / `rdma_mcrdma3` | `enp1s0f1np1` / `rocep1s0f1`, GID index 1 (RoCE v2) |

Physical link: one NVIDIA QSFP112 DAC, negotiated 100GBASE-CR4 with RS-FEC on both ends. Ethernet MTU 9000 on both ends. On the peer, NetworkManager was told to stop managing the port before the MTU, address and neighbor steps, because its DHCP retries otherwise undo them. No switch.

A second card was attached to the Mac during these runs: a ConnectX-4 Lx `15b3:1015` dual-port adapter on a separate Thunderbolt port. The driver also bound it (`mcrdma0`, `mcrdma1`). It had no cable and carried no traffic. The checker listed seven RDMA devices: three of Apple's own and four native ConnectX ports (two per card; `mcrdma2` is the uncabled second CX-5 port). It reported `native_cx5=4`, one active port and no errors.

## Build identity

Source: this repository at `7192192`, built on the Mac at 17:36 UTC with Xcode 27.0 (`27A266a`) and the macOS 27.0 SDK. The peer binaries were built from the same commit with GCC 13.3.0.

| Item | Identity |
|---|---|
| Kernel extension `org.mcdma.cx5.native` 0.1.18 | UUID `6BB0406E-E8D6-3965-A610-EC6F788AEFC7` |
| Provider `libmcdma-rdmav34.so` (installed copy = build copy) | SHA-256 `fc33ceb1d2e0af31b223b059b267ffea78afef40e145c81920a5f588110194fd` |
| Mac `native-verbs-peer` | SHA-256 `a9c4072965990e974195f80c57d7def2930cae2c103bc60e8fcaf60a94bea1fb` |
| Mac `mcdma-bw` | SHA-256 `de919c4906b62cb29a847c1a26b8fe965f054569edfe6f20ff0b9b97a5057d0f` |
| Mac `cx5-native-check` | SHA-256 `d87910f148655b16c3c694bde248aa7af9ed7d4fbe80061f8b2bc5ad821ed092` |
| Peer `verbs-peer` (GCC 13.3.0) | SHA-256 `d51058fa10e038aadcc3801dfd35bd9fdf4dd3ba8b7f992eb66b54349b0896a3` |
| Peer `mcdma-bw` (GCC 13.3.0) | SHA-256 `64d3b2f93022c506faefb12b2134e3bb79894d34fe4589f1d73ed41e34aeb8b9` |

## Correctness

`cx5-native-check --require-gid` passed on `rdma_mcrdma3` (port active, RoCE v2 link-local GID, no errors). Then `tools/native_cross_host.py` with 4 KiB payloads and RC path MTU 4096, one arm at a time, in the documented order:

| Arm | CQ map / user post / BlueFlame | Mode markers | Mac WRITE | Mac READ | GX10 WRITE | GX10 READ |
|---|---|---|---|---|---|---|
| Kernel posting | 0 / 0 / 0 | kernel, as requested | verified | verified | verified | verified |
| Direct posting | 2 / 1 / 0 | CQ mapped, user post confirmed | verified | verified | verified | verified |
| BlueFlame-64 | 2 / 1 / 64 | user post and BlueFlame confirmed, `uar_wc=1` | verified | verified | verified | verified |

Each "verified" is 4096 bytes checked by the runner. No arm fell back to a different posting mode.

## Latency

The README command, BlueFlame-64, RC path MTU 1024, 1000 timed operations per verb, **Metal keepalive off**. Each configuration ran twice with fresh output files. These are completion times for one operation at queue depth one, not one-way wire latency.

| Payload | Operation | Run 1 median / p99, µs | Run 2 median / p99, µs | README Studio median, µs (1) |
|---|---|---|---|---:|
| 4 KiB | Mac WRITE | 9.96 / 19.75 | 9.83 / 16.96 | 7.625 |
| 4 KiB | Mac READ | 8.02 / 11.17 | 7.75 / 11.50 | 6.042 |
| 4 KiB | GX10 WRITE | 3.17 / 3.33 | 3.12 / 4.10 | 3.680 (Spark) |
| 4 KiB | GX10 READ | 6.54 / 7.09 | 6.62 / 7.09 | 5.536 (Spark) |
| 1 KiB | Mac WRITE | 8.88 / 14.08 | 8.50 / 15.08 | not published |
| 1 KiB | Mac READ | 6.42 / 11.38 | 6.50 / 11.42 | not published |
| 1 KiB | GX10 WRITE | 2.46 / 2.83 | 2.66 / 3.30 | not published |
| 1 KiB | GX10 READ | 5.60 / 6.32 | 6.13 / 7.15 | not published |

(1) Copied from the README headline table, not re-measured: Mac Studio M3 Ultra and one DGX Spark, lab driver 0.1.17, continuous Metal keepalive on, 40 Gb/s link, pooled over three runs. Our runs differ in the Mac, the driver version (0.1.18), the keepalive (off) and the link rate (100 Gb/s), so the column is context, not a matched comparison.

All 8,000 Mac samples and 8,000 GX10 samples (4 runs x 2 verbs x 1000) completed. These four runs started five to six minutes after the Mac booted. Later runs on the same boot were faster; see the next section.

### Later runs and a keepalive A/B

From 19:18 UTC, 19 to 21 minutes after boot, the same 4 KiB / MTU 1024 / BlueFlame-64 command ran nine more times: three keepalive-off and three keepalive-on runs alternated (off, on, off, on, off, on), then three more keepalive-off runs. The keepalive was `fabric-keepalive 0 small`, started 3 s before each "on" run and stopped after it. All nine runs passed. Medians, µs:

| Run | Keepalive | Mac WRITE | Mac READ | GX10 WRITE | GX10 READ |
|---|---|---:|---:|---:|---:|
| A1 | off | 7.708 | 5.542 | 3.120 | 5.760 |
| A2 | on | 7.542 | 5.667 | 3.040 | 5.664 |
| A3 | off | 7.833 | 5.958 | 3.120 | 5.856 |
| A4 | on | 7.708 | 5.875 | 3.120 | 5.536 |
| A5 | off | 7.833 | 6.042 | 3.576 | 6.144 |
| A6 | on | 7.708 | 5.958 | 3.120 | 5.744 |
| A7 | off | 7.750 | 6.000 | 3.328 | 5.808 |
| A8 | off | 7.791 | 6.021 | 3.312 | 5.808 |
| A9 | off | 7.333 | 5.500 | 3.312 | 6.032 |

On this MacBook the keepalive made no clear difference: its on-off gaps are smaller than the spread between off runs. That differs from the Studio result in [gpu-keepalive.md](gpu-keepalive.md). The comparison was not controlled: other applications were running during all nine runs (the busiest used 130 to 145% CPU, and WindowServer about 46%), and their GPU activity may already have kept the platform in the faster state. A quiet-machine A/B was not run.

The Mac-initiated medians fell by about 2 µs between the 19:04 runs and these. The cause was not isolated; time since boot is the only recorded difference. For reference, the Studio's 100 Gb/s single runs in [gpu-keepalive.md](gpu-keepalive.md#100-gbs-link-runs) at the same MTU and payload were 7.21 to 7.29 µs WRITE and 5.92 µs READ from the Studio with keepalive on, and 9.88 / 7.12 µs with it off (driver 0.1.17).

## Sustained bandwidth

The command from [Reproducing the bandwidth shape](validation-2026-09-17.md#reproducing-the-bandwidth-shape), unchanged apart from hosts, paths and devices: kernel posting, one RC QP, queue depth one, one 4 MiB slot, 8 GiB per trial, one warmup before each measured trial, three rounds, path MTU 4096, flag finish, 1 MiB verification budget. All 12 measured trials passed with confirmed modes, no completion errors, no sampled-byte mismatches and guards intact. Initiator rows with `warmup=0`:

| Payload direction | Initiator and operation | Three runs, Gbit/s | Median, Gbit/s | 17 Sep Studio median, Gbit/s |
|---|---|---|---:|---:|
| GX10 to MacBook | Mac READ | 50.5, 50.5, 50.5 | 50.5 | 50.5 |
| GX10 to MacBook | GX10 WRITE | 50.9, 50.9, 50.9 | 50.9 | 51.0 |
| MacBook to GX10 | Mac WRITE | 28.1, 27.3, 26.4 | 27.3 | 29.4 |
| MacBook to GX10 | GX10 READ | 26.2, 28.4, 25.9 | 26.2 | 24.2 |

The right-hand column is copied from the 17 September report for reference; it was not re-measured here. As in that report, correctness is sampled (12,288 bytes after each trial), not every transferred byte. The initiator busy-polls and used about one CPU core.

The directional shape of the 17 September Studio runs appears on this MacBook as well: about 50.5 Gbit/s into the Mac and 26 to 28 Gbit/s out of it. That points away from the Studio specifically. It does not identify the cause.

### Repeat sweep and longer runs

The same 8 GiB sweep, repeated at 19:22 UTC with a fresh output directory, gave medians of 50.5 (Mac READ), 50.9 (GX10 WRITE), 26.5 (Mac WRITE) and 26.1 (GX10 READ) Gbit/s. All 12 trials passed with the same checks.

Longer single transfers followed the 17 September follow-up: one 4 MiB slot, depth one, kernel posting, no warmup, one run per configuration unless listed twice.

| Payload direction | Initiator and operation | Total payload | Duration | Gbit/s | 17 Sep Studio, Gbit/s |
|---|---|---:|---:|---:|---:|
| GX10 to MacBook | Mac READ | 96 GiB | 16.314 s | 50.5 | 50.6 |
| MacBook to GX10 | Mac WRITE, run 1 | 64 GiB | 16.743 s | 32.8 | 29.4 |
| MacBook to GX10 | Mac WRITE, run 2 | 64 GiB | 20.110 s | 27.3 | |
| MacBook to GX10 | GX10 READ, run 1 | 64 GiB | 16.685 s | 32.9 | 24.6 |
| MacBook to GX10 | GX10 READ, run 2 | 64 GiB | 21.416 s | 25.7 | |

All passed with no completion errors, no sampled-byte mismatches, intact guards and confirmed modes. Into the Mac, every run landed at 50.5 to 51.0 Gbit/s. Out of the Mac, results ranged from 25.7 to 32.9 Gbit/s between runs of the same configuration a few minutes apart. The two 32.8 / 32.9 results were not reproduced by the repeats, so they show the spread, not a new outbound rate.

## In context: the same direction over TCP

What a Mac-to-GB10 link delivered on our bench before MCDMA, next to the RDMA medians above. The TCP rows use a different driver, protocol and MTU; they show what MCDMA replaced for us, and are not MCDMA measurements or a like-for-like comparison.

| Path, Mac to GB10-class peer | Into the Mac, Gbit/s | Out of the Mac, Gbit/s |
|---|---:|---:|
| MCDMA RDMA, this MacBook, Helios 5S + CX-5 Ex (Mac READ / Mac WRITE, first 8 GiB sweep; outbound runs ranged 25.7 to 32.9) | 50.5 | 27.3 |
| MCDMA RDMA, 17 Sep Studio report (Studio READ / Studio WRITE) | 50.5 | 29.4 |
| TCP, this MacBook, same card, cable and GX10 port under Apple's Ethernet driver, `iperf3`, 1 / 4 streams (2) | 20.7 / 21.2 | 20.0 / 28.7 |
| TCP, this MacBook over 10 GbE (QNAP QNA-T310G1T, Aquantia AQC107) to a GX10 (3) | not recorded | 9.36, 8.77 |

(2) Measured 23 September 2026 at 18:25 UTC, before installing MCDMA, 10 s per run, MTU 1500 (Apple's driver caps this port at 2034). Earlier runs the same day on the other GX10 port gave 25.9 / 29.1 out and 19.0 / 13.3 in, so single TCP runs vary by several Gbit/s.
(3) Measured 17 September 2026. The outbound figures are file copies, not `iperf3`. A separate `iperf3` run gave about 9.4 Gbit/s, but its direction was not recorded, so it is not placed in either column.

## A model over this link: remote prefill with oMLX

Later the same evening (19:58 to 20:35 UTC) the link carried a real model handoff: [jundot/omlx#3870](https://github.com/jundot/omlx/pull/3870) on the MacBook decoding from this repository's vLLM connector (`integrations/vllm/mcdma_kv`) on the GX10, through `mcdma-rpcd` (one link, 4 MiB request and 64 MiB reply halves). Both sides ran `Qwen/Qwen3-4B-Instruct-2507` in BF16: vLLM 0.27.1 in NVIDIA's `nvcr.io/nvidia/vllm:26.08-py3` container on the GX10, oMLX 0.7.0.dev4 at the #3870 head on the Mac. To our knowledge this is the first run of that path on hardware.

Method: a needle-in-a-haystack prompt with a fresh random filler and passphrase for every request, so no prefix cache could help. The prompt asked for the passphrase and then a story of about 300 words, so every reply ran to the 128-token cap. Greedy decoding, streamed; times are measured by the client end to end. Three configurations, three requests per prompt length, medians:

- **Mac-only:** oMLX prefills and decodes.
- **GX10-only:** vLLM prefills and decodes.
- **Split:** vLLM prefills on the GX10, the KV cache comes back over MCDMA, oMLX decodes. Checksums were on (the default).

The split runs used the connector with one change, python-isal's CRC-32 in place of zlib's, from [#5](https://github.com/ashhart/MCDMA/pull/5); see the next section. All 45 requests returned exactly 128 tokens and the correct passphrase. In every split request oMLX reported all but the last three prompt tokens as coming from the transferred cache.

| Prompt tokens | Mac-only first token, s | GX10-only first token, s | Split first token, s | Mac decode, tok/s | GX10 decode, tok/s | Split decode, tok/s |
|---:|---:|---:|---:|---:|---:|---:|
| 1,035 | 1.55 | 0.19 | 0.31 | 60.4 | 21.9 | 59.6 |
| 3,830 | 0.74 | 0.77 | 0.88 | 57.6 | 21.1 | 58.2 |
| 7,600 | 1.65 | 1.30 | 1.68 | 55.3 | 20.0 | 55.4 |
| 15,141 | 4.32 | 2.71 | 3.54 | 50.4 | 18.3 | 50.7 |
| 28,270 | 10.92 | 6.51 | 7.99 | 42.7 | 15.9 | 42.8 |

| Prompt tokens | Mac-only 128-token reply, s | GX10-only, s | Split, s |
|---:|---:|---:|---:|
| 1,035 | 3.73 | 6.00 | 2.44 |
| 3,830 | 2.95 | 6.76 | 3.06 |
| 7,600 | 3.95 | 7.60 | 3.96 |
| 15,141 | 6.84 | 9.66 | 6.04 |
| 28,270 | 13.90 | 14.51 | 10.93 |

What this shows, for this model and these three runs per point:

- The split kept the Mac's decode rate, about 2.7 times this GX10 baseline, and moved prefill to the GX10. At 28,270 tokens its 128-token reply took 21% less time than Mac-only, and at 15,141 tokens 12% less. At 3,830 and 7,600 tokens the configurations were within a few percent of each other.
- The 1,035-token Mac-only first token (1.55 s, longer than at 3,830 tokens) appeared in two of three Mac-only requests and was not investigated, so the split's apparent lead at that length is not claimed.
- The M5 Max prefilled 28,270 tokens in about 10.9 s. The [disaggregated-inference report](disaggregated-inference.md) gives 22.32 s for 28,852 tokens on the M3 Ultra Studio, with an MXFP4 checkpoint and summed stage times. The quantisation and the method differ, so this is context, not a matched comparison; it is why the split gains less on this Mac than in that report.
- The GX10 decode figures are BF16 in an untuned vLLM configuration and should not be read as the GB10's best decode rate.

### Tuning the split, 24 September

On 24 September between 02:27 and 02:37 UTC the split was rerun at 15,137 and 28,267 prompt tokens, three fresh prompts each, with the same client and 128-token replies. All other settings were unchanged except as listed. Every split row was accepted only if oMLX logged a completed remote handoff for that request, and each of the 18 split requests did. Before these runs, removing a second ConnectX card from the Mac had renumbered the MCDMA interfaces (`mcrdma3` became `mcrdma1`). That dropped the link until `tools/restore-rdma.py` was rerun on the new name. Requests during the outage fell back to local prefill and are excluded.

| 28,267-token prompt | First token, s | 128-token reply, s | GX10 prefill, s (oMLX log) | Transfer, s |
|---|---:|---:|---:|---:|
| Split as above (vLLM default `max_num_batched_tokens` 2048) | 7.99 | 10.93 | 6.30 to 6.70 | 1.18 to 1.28 |
| `--max-num-batched-tokens 16384` | 7.49 | 10.54 | 5.95 to 6.01 | 1.11 to 1.14 |
| 16384, checksums off | 7.48 | 10.42 | 6.18 to 6.23 | 0.87 to 0.88 |
| 16384, `--quantization fp8` on vLLM | 8.24 | 11.20 | 6.65 to 6.66 | 1.22 to 1.25 |

With larger prefill chunks the split's reply took 24% less time than the Mac-only 13.90 s. At 15,137 tokens it was 5.88 s against 6.84 s. Turning checksums off shortened the transfer but not the first token in these three runs. Online FP8 weights roughly doubled vLLM's own decode rate (25.0 to 43.9 tok/s over the five lengths) but made the 28k prefill slower, so the split lost time; FP8 accuracy was not evaluated beyond the passphrase check. The remaining transfer, about 1.1 s at 28k, is the part that streaming layers during prefill would hide. That was not attempted.

### Next to the Studio + Spark run

The [disaggregated-inference report](disaggregated-inference.md) reports the same kind of split on an M3 Ultra Studio and a DGX Spark. Its numbers are copied below, not re-measured. The setups differ in several ways: its checkpoint was MXFP4 where ours was BF16, its driver was 0.1.17, and its reply times are sums of separately measured stages where ours are end-to-end client times. Its prompt lengths also differ from ours by 2 to 5%. Times are 128-token replies with the first token in brackets; the percentage is how much less time the split took than Mac-only.

| Prompt, ours / Studio run | Ours: Mac-only, s | Ours: split, s | Ours: less time | Studio run: Studio-only, s | Studio run: split, s | Studio run: less time |
|---|---:|---:|---:|---:|---:|---:|
| 1,035 / 977 | 3.73 (1.55) | 2.44 (0.31) | not claimed (see above) | 1.19 (0.39) | 1.05 (0.20) | 12% (49%) |
| 3,830 / 3,852 | 2.95 (0.74) | 3.06 (0.88) | -4% (-19%) | 2.50 (1.63) | 1.84 (0.76) | 26% (53%) |
| 7,600 / 7,702 | 3.95 (1.65) | 3.96 (1.68) | 0% (-2%) | 4.62 (3.62) | 2.97 (1.56) | 36% (57%) |
| 15,141 / 15,402 | 6.84 (4.32) | 5.88 (3.36) | 14% (22%) | 10.03 (8.81) | 5.55 (3.52) | 45% (60%) |
| 28,270 / 28,852 | 13.90 (10.92) | 10.54 (7.49) | 24% (31%) | 23.94 (22.32) | 11.21 (8.08) | 53% (64%) |

The two lower rows of our split use the 16k prefill chunks from the tuning runs; the others use vLLM's default. In absolute time the two splits were close: 10.54 s and 11.21 s at the longest prompt. The percentages differ mainly because of the Mac-only baseline. The M5 Max prefilled about twice as fast as the Studio run's M3 Ultra, which leaves the remote prefill less to save.

The same runs as rates. Effective prefill is prompt tokens divided by the time to the first token, so for the splits it includes the handoff. Decode is the streamed rate after the first token, which the split does not change. Prefill rates are rounded to the nearest token per second.

| Prompt, ours / Studio run | Ours: Mac-only prefill, tok/s | Ours: split prefill, tok/s | Studio run: Studio-only prefill, tok/s | Studio run: split prefill, tok/s | Ours: Mac / GX10 decode, tok/s | Studio run: Studio / Spark decode, tok/s |
|---|---:|---:|---:|---:|---:|---:|
| 3,830 / 3,852 | 5,176 | 4,352 | 2,363 | 5,068 | 57.6 / 21.1 | 147 / 60 |
| 7,600 / 7,702 | 4,606 | 4,524 | 2,128 | 4,937 | 55.3 / 20.0 | 131 / 53 |
| 15,141 / 15,402 | 3,505 | 4,506 | 1,748 | 4,376 | 50.4 / 18.3 | 109 / 42 |
| 28,270 / 28,852 | 2,589 | 3,774 | 1,293 | 3,571 | 42.7 / 15.9 | 83 / 31 |

The Studio run's decode rates are for an MXFP4 checkpoint and ours for BF16, which reads about four times as many weight bytes per token. The decode columns therefore compare the two setups as run, not the machines.

### MXFP4 on the Mac

The Studio run's model was MXFP4, and at the same quantisation the decode rates can be compared directly. On 24 September (02:51 to 02:57 UTC) the Mac's copy of the model was converted with mlx-lm 0.32.0 (`mlx_lm convert -q --q-mode mxfp4 --q-group-size 32 --q-bits 4`, 4.25 bits per weight). The GX10 kept prefilling in BF16 with 16k chunks, because the Studio run's matching compressed-tensors converter is not published. The split therefore decodes an MXFP4 model from a cache that a BF16 model computed, which is a different computation from the Studio run's all-MXFP4 run. All 15 measured split requests, and the 2 warmups before them, logged a completed handoff with no failures. Medians of three, 128-token replies; best value per row in bold:

| Prompt, ours / Studio run | Prefill tok/s: ours Mac-only / ours split / Studio-only / Studio + Spark split | Decode tok/s: ours Mac / Studio | Output tok/s over the whole reply: ours Mac-only / ours split / Studio-only / Studio + Spark split |
|---|---|---|---|
| 3,831 / 3,852 | 4,893 / 4,362 / 2,363 / **5,068** | **155.1** / 147 | **79.9** / 75.8 / 51.2 / 69.6 |
| 7,600 / 7,702 | 4,353 / 4,811 / 2,128 / **4,937** | **133.3** / 131 | 47.4 / **50.5** / 27.7 / 43.1 |
| 15,140 / 15,402 | 3,409 / **4,454** / 1,748 / 4,376 | 106.1 / **109** | 22.7 / **27.6** / 12.8 / 23.1 |
| 28,270 / 28,852 | 2,652 / **3,735** / 1,293 / 3,571 | 76.5 / **83** | 9.9 / **13.9** / 5.3 / 11.4 |

At the same quantisation the M5 Max decoded within about 8% of the M3 Ultra at every length, so the BF16 gap above was the weights. At 28,270 tokens the split's reply took 9.24 s against 12.90 s Mac-only (28% less time). The Studio run's split took 11.21 s at 28,852 tokens. 28 of 30 answers were correct. The two misses were a digit off in the passphrase, one in each configuration. Replaying both prompts in both configurations, with oMLX's prefix cache moved aside, reproduced the misses in all four runs, with the same wrong passphrase at 28k in both. The misses are therefore the 4-bit model on those two prompts, not the handoff. The 1,035-token rows are omitted here for the same reason as above.

### What changed from the untuned run

1. The producer's CRC-32 uses python-isal instead of zlib (#5): checked transfers went from 16 to 19 Gbit/s to 26 to 30.
2. vLLM `--max-num-batched-tokens 16384`: the GX10's 28k prefill went from about 6.4 s to 6.0 s.
3. The MXFP4 Mac model decodes 1.8 to 2.9 times as fast as BF16 (42.7 to 76.5 tok/s at 28k, 60.4 to 177.8 at 1k) and leaves prefill almost unchanged.

Checksums were left on. Turning them off gave no further gain after change 2, and online FP8 on vLLM made prefill slower.

### The producer checksum limited the handoff

With per-frame checksums on and the connector as published, oMLX logged KV transfers of 16 to 19 Gbit/s. For example, 28,268-token handoffs took 1.78, 2.06 and 2.07 s. With `OMLX_REMOTE_PREFILL_CHECKSUM=0` the same handoffs took 0.94 to 0.96 s, about 35 Gbit/s. The responder computes `zlib.crc32` serially for each frame, and in this container zlib's CRC-32 ran at 6.4 GB/s on the GB10. The Mac's ran at 42 GB/s. python-isal computes the same CRC-32 at 18.9 GB/s there. With it, checked handoffs took 1.24 to 1.28 s (26 to 27 Gbit/s), and the 28k first token went from 8.74 s (zlib, median of three) to 8.08 s. With checksums off it was 7.70 s. That change, with tests for both code paths, is in [#5](https://github.com/ashhart/MCDMA/pull/5).

## Not covered

One link only; the second CX-5 port was not cabled. No concurrent-port runs, no process-termination matrix, no keepalive A/B on an otherwise idle machine, no GPU-buffer paths, one small model only (above), and no long soak. The MacBook was on AC power, battery full, default power mode (checked with `pmset` after the runs, not controlled during them). Raw CSVs, manifests and logs are kept outside this repository because they contain addresses and memory-region keys.
