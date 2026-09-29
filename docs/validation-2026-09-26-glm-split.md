# GLM-5.3-Flash split across two Linux hosts, RDMA against TCP, 26 September 2026

This report uses llama.cpp's own RPC transport and its built-in RDMA path (ggml-org/llama.cpp #20590). No MCDMA
code is involved. It sits next to the [KV handoff report](validation-2026-09-26-kv-handoff.md) because it ran on the
same two hosts and the same link, and it answers the question that report leaves open: what RDMA does for a model
split layer by layer, rather than for moving a whole cache.

## Setup

- Model: unsloth `GLM-5.3-Flash-UD-Q3_K_XL`, four shards, 137.39 GiB (llama-bench: `glm5next 313B.A17B Q3_K -
  Medium`, 320.76 B parameters). Too large for either host's 128 GB alone.
- llama.cpp: upstream master 81bc6b83f with the GLM-5.3 pull request #27754 (head 86ebfef2c) merged, commit
  0b5f560a6. Upstream master cannot load this architecture yet. Both hosts built the same source.
- Host A (Strix Halo, Radeon 8060S, Vulkan): runs `llama-bench` with `--rpc` and holds the model file.
- Host B (GB10, CUDA): runs `ggml-rpc-server -d CUDA0` on its end of the link.
- Link: the pair from the [Linux pair report](validation-2026-09-26-linux-pair.md), ConnectX-5 Ex over USB4 to
  ConnectX-7, 100 Gb/s, netdev MTU 9000, RoCE v2 active MTU 4096.
- TCP rows: `GGML_RPC_NO_RDMA=1` on both ends. RDMA rows: default; the client log records `RDMA activated ...
  mtu=4096`.
- `-ngl 99 -fa 1`. `ts` is the layer split host A / host B. Values are llama-bench's mean and deviation: two
  repetitions per row (`-r 2`) except the final-settings table, which used three (`-r 3`). The 50/50 TCP rows are
  llama.cpp's default split with no `-ts` (about 50.2/49.8, set by free memory); every other split was set explicitly.

## RDMA against TCP, same settings

| Split A/B | Transport | pp512 (t/s) | tg128 (t/s) |
|---|---|---:|---:|
| 50/50 (default) | TCP | 213.97 ± 3.65 | 13.44 ± 0.08 |
| 50/50 | RDMA | 212.79 ± 6.15 | 13.85 ± 0.03 |
| 75/25 | TCP | 254.85 ± 0.41 | 12.81 ± 0.00 |
| 75/25 | RDMA | 255.97 ± 1.46 | 13.47 ± 0.01 |

Generation is 3.1% faster over RDMA at 50/50 and 5.2% at 75/25; prompt processing is unchanged within its
deviation. Two repetitions per row is a small sample: the difference is larger than the reported deviations, but it
is not a general result for other models, splits or links. On the same pair, a split of Qwen3.8-27B measured the
same over both transports (426 and 430 t/s pp512, 11.1 and 11.2 t/s generation).

## Layer split, TCP

| Split A/B | pp512 (t/s) | tg128 (t/s) |
|---|---:|---:|
| 25/75 | 171.39 ± 2.62 | 12.00 ± 0.01 |
| 30/70 | 183.14 ± 5.54 | 12.31 ± 0.18 |
| 40/60 | 201.40 ± 5.47 | 13.01 ± 0.04 |
| 50/50 (default) | 213.97 ± 3.65 | 13.44 ± 0.08 |
| 60/40 | 233.55 ± 0.16 | 13.26 ± 0.01 |
| 70/30 | 245.76 ± 0.52 | 12.90 ± 0.00 |
| 75/25 | 254.85 ± 0.41 | 12.81 ± 0.00 |

Moving layers to host B, the faster host on its own, lowered both rates. Prompt processing rose steadily with host
A's share; generation peaked at 50/50.

## Batch size and final settings, RDMA only

`-ub` at 50/50, pp2048: 512 gave 263.41 ± 1.32, 1024 gave 299.10 ± 1.01, 2048 gave 257.84 ± 1.26 t/s.

| Split A/B, ub 1024, `-r 3` | pp2048 (t/s) | tg128 (t/s) |
|---|---:|---:|
| 50/50 | 299.06 ± 0.45 | 13.77 ± 0.04 |
| 60/40 | 351.47 ± 2.68 | 13.89 ± 0.01 |
| 75/25 | 385.92 ± 1.14 | 13.43 ± 0.01 |

These rows use a longer prompt test (pp2048) than the tables above (pp512), so they are not comparable with them
row for row, and none of them has a TCP counterpart. They show the best settings found, not a transport effect.

## Not covered

More than two or three repetitions, TCP at the final settings, other models or quantisations, concurrent requests, and output
comparison between transports beyond a coherent temperature-0 answer.
