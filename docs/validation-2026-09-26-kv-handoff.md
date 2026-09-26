# KV handoff between two Linux llama.cpp hosts, 26 September 2026

One llama.cpp server prefills a prompt and saves its slot (the KV cache and recurrent state) to a file; a second
server on another machine restores that file and decodes. The file crossed a direct 100 Gb/s RoCE v2 link three
ways: mcdma-bw resident payload runs, one TCP stream, and a running `mcdma-rpcd` link (one or two links in
parallel). Tools: `benchmarks/llamacpp_kv_handoff.py`, `benchmarks/rpc_file.py`, `benchmarks/rpc_roundtrip.py`.
The same pair was characterised with `run_bw.py` in the [Linux pair report](validation-2026-09-26-linux-pair.md).

## Setup

| | Producer (host B) | Consumer (host A) |
|---|---|---|
| Machine | ASUS Ascent GX10 (GB10) | AMD Strix Halo mini PC |
| NIC path | ConnectX-7, internal, firmware 28.45.4028 | ConnectX-5 Ex in an OWC Helios 5S over USB4, firmware 16.35.8002 |
| Kernel / rdma-core | 7.0.0-1019-nvidia / 50.0-2ubuntu0.2 | 7.0.0-34-generic / 61.0-2ubuntu3 |
| llama.cpp backend | CUDA | Vulkan |

Both servers ran the same build (81bc6b8) and the same `Qwen3.8-27B-UD-Q4_K_XL.gguf` with `-c 16384 -np 1
--slot-save-path` on tmpfs. Link: one passive 200G QSFP56 copper cable, negotiated 100 Gb/s, netdev MTU 9000 on
both ends. RoCE v2 over IPv4-mapped GIDs. mcdma-bw runs used RDMA path MTU 1024; the `mcdma-rpcd` links 4096.

Each round: the producer prefills N-1 tokens and saves the slot; the file crosses once per transport, in rotating
order; after each crossing both ends' SHA-256 are compared, the consumer erases its slot, restores the file and
completes all N tokens with 64 greedy tokens. Host A's locked-memory limit was raised from 8 MiB (the Linux pair
report) to unlimited before these runs.

The wire boundaries differ by transport, so wire times compare only roughly:

- mcdma-bw: the initiator's data loop from the resident source buffer into the peer's registered buffer, summed
  over chunks. It excludes reading the file into the buffer and writing the dump.
- TCP: the receiver, from its first byte to EOF, including its writes to tmpfs. The sender uses `sendfile`.
- `mcdma-rpcd`: the consumer's pull loop, including the service's read of each frame from tmpfs into the mailbox
  and the consumer's write of it to tmpfs.

Wall time is the whole transfer step as the orchestrator saw it, including SSH, and is the comparable figure.

## Results

Slot files: 291,104,292 bytes at 2048 tokens, 693,929,508 bytes at 8192. Every one of the 24 crossings below
arrived with matching SHA-256, and every restore left the consumer one prompt token to evaluate (`prompt_n` 1).
Three rounds each; ranges are min to max.

| Run (UTC) | Prompt | Transport | Wire s | Wire Gbit/s | Wall s |
|---|---:|---|---:|---:|---:|
| 15:17 | 2048 | mcdma-bw payload, 1 chunk | 0.079 | 29.4 | 5.21-5.30 |
| | | TCP | 0.139-0.151 | 15.4-16.7 | 0.59-0.60 |
| 15:19 | 8192 | mcdma-bw payload, 1 chunk | 0.184-0.185 | 30.0-30.1 | 10.05-10.49 |
| | | TCP | 0.325-0.386 | 14.4-17.1 | 0.85-1.38 |
| 15:34 | 8192 | `mcdma-rpcd`, one link | 0.456-0.476 | 11.7-12.2 | 0.83-1.02 |
| | | TCP | 0.319-0.346 | 16.0-17.4 | 0.90-0.95 |
| 15:52 | 8192 | `mcdma-rpcd`, two links | 0.249-0.289 | 19.2-22.3 | 0.54-0.76 |
| | | TCP | 0.377-0.442 | 12.6-14.7 | 1.04-1.12 |

The 15:52 run overlapped a large download on the consumer: its decode fell from 10.2 to 8.3-8.5 tok/s and its own
full prefill from about 37.5 s to 52.5 s. Transports alternated within each round, but the load was not controlled.

Consumer side, 8192 tokens: restore 80-117 ms, first token 145-279 ms after restore, decode 8.3-10.2 tok/s.
For the same prompt the consumer alone took 37.4-38.4 s to prefill (15:19 and 15:34 baselines), and the producer
9.84-9.89 s.

## Observations

- **Per-run setup, not the wire, decided the first two runs.** mcdma-bw moved the file at the link's ~30 Gbit/s,
  twice TCP's rate, but each run spent about 5 s on SSH, preflight and queue pair setup, per chunk. It is a
  measurement tool, not a transport.
- **A persistent link removes that setup, and a single link runs in series.** Protocol 1 carries one call per
  link at a time, so each 16 MiB frame was read from the producer's file, sent and written on the consumer
  strictly in turn: 12 Gbit/s. Two links pulling halves concurrently overlapped the copies with the wire:
  19-22 Gbit/s and the only arm faster than TCP end to end: 0.54-0.76 s wall against 1.04-1.12 s for the TCP arm
  of the same run (about 1.7x). That TCP arm ran during the download described above. Against the unloaded TCP
  arms of the 15:19 and 15:34 runs (medians 0.85 and 0.92 s) the two-link median of 0.62 s is about 1.4x faster,
  and the two-link arm itself ran under the download.
- **Echo round trips on the same link** (`rpc_roundtrip.py`, 1000 calls a size, 0 mismatches; throughput from the
  mean latency): 11.1 us median for 64 B at path MTU 1024; at path MTU 4096 with a cached reply, 1.29 ms median
  for 4 MiB (26.0 Gbit/s) and 3.82 ms for 12 MiB (26.3 Gbit/s). Two things changed between those runs (path MTU
  and the cached reply), so the improvement from the first run's 9.6 Gbit/s is not attributed to either alone.
  The cached-reply run sent the same bytes for every call of a size, so it could not have detected a stale reply;
  the service now stamps each reply with its call's sequence number, and that run was not repeated.
- **The handoff is byte-correct; the decoded text differs across backends.** The consumer's 64 tokens never
  equalled the producer's own continuation or the consumer's own full prefill. A same-host control on the
  producer (save, erase, restore, decode, against its own full prefill, a 2526-token varied prompt) matched
  exactly with `prompt_n` 1. With the file's hash equal on both ends, the difference is the CUDA-produced cache
  decoded by Vulkan, not the transfer.
- **Where it pays.** Handing off cut the consumer's time to first token for 8192 tokens from about 37.5 s to about
  11 s (producer prefill 9.9 s, save 0.38 s, transfer 0.54-0.76 s, restore 0.1 s, first token 0.2 s). The producer
  here also decodes faster than the consumer (12.3-12.5 against 10.2 tok/s), so for one request it would be faster
  still to let the producer finish it. The handoff pays when the decoding host is the faster decoder or the
  producer is needed for the next prompt.
- **Slot size.** 2048 and 8192 tokens gave about 157 MB of fixed state plus about 65 KB per token for this
  checkpoint.

## Not covered

More than two links, larger reply halves, a C client, streaming layers during prefill, same-backend pairs,
concurrent requests, and anything but this one checkpoint. Raw data (per-arm JSON lines, `results.json`, chunk
logs, console output) was kept with the runs; one attempt that failed with HTTP 503 while the consumer server was
still loading is not a result.
