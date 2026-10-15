# Disaggregated inference: Qwen3.8 Flash Next, prefill on a Spark, decode on a Mac Studio

Since 10 October 2026 one Spark and one Mac Studio have served **Qwen3.8 Flash Next** as a single OpenAI-compatible
model. vLLM on the Spark prefills every prompt of 4,096 or more uncached tokens. The [KV handoff](kv-handoff.md)
connector exports the model's whole-prefix state, the Mac pulls it over the CX5 link, and oMLX decodes the reply with
the model's MTP head drafting. Unlike the [Qwen3-4B note](disaggregated-inference.md), nothing is staged in files:
the connector runs inside vLLM, the pull runs inside oMLX's scheduler, and each step below is code you can run. This
page is both a recipe and a report, with the numbers and every limitation.

Code:

- **MCDMA:** this repository's vLLM connector, `integrations/vllm/mcdma_kv` (hybrid and MTP export:
  `hybrid.py`), and the link daemon.
- **oMLX:** branch [`feat/remote-prefill-hybrid`](https://github.com/Aileuxe/omlx/tree/feat/remote-prefill-hybrid)
  on Aileuxe/omlx, built on ashhart/omlx `feat/remote-prefill`.

## What makes this model harder than Qwen3-4B

Flash Next (`qwen4_exp`) is a hybrid of three kinds of layer:

- 36 Gated DeltaNet layers with recurrent state;
- 12 QSA sparse-attention layers with a learned indexer;
- one PLE short-convolution layer that reads a 51 GB n-gram table.

A recurrent state only exists for a whole prefix. So a handoff cannot append pages to what the decoder already has:
it must carry every layer's state at the end of the prompt.

- **QSA layers:** keys, values, and the indexer's raw keys. vLLM keeps the raw keys only for the last four tokens, so
  the connector records them during prefill.
- **Gated DeltaNet layers:** the conv window and the float32 recurrent state.
- **PLE layer:** its conv window.

[KV handoff](kv-handoff.md#hybrid-models) gives the exact layout. On the Mac the state replaces whatever the local
prefix cache restored.

The decoder also drafts with the model's one-layer MTP head, and that head has a cache of its own. oMLX normally fills
it while prefilling: "prompt priming", which feeds the head the trunk's hidden state at each prompt position. When the
Spark prefills, the Mac never sees those hidden states. The fix is for vLLM to run the same MTP drafter during prefill
and send its cache, plus the hidden state of the last prompt token, along with the rest.

## Hardware and software

| | Decode end | Prefill end |
|---|---|---|
| Machine | Mac Studio M3 Ultra, 256 GB | Lenovo ThinkStation PGX (GB10, 128 GB) |
| OS | macOS 27.2 beta `26B5101f`, SIP off and Reduced Security | Ubuntu 24.04 (DGX OS), kernel 7.0 `-nvidia` |
| NIC | ConnectX-5 Ex in an OWC Helios 5S on Thunderbolt 5 | built-in ConnectX-7, one port |
| Link | 40G QSFP+ cable, RoCE v2, MTU 9000 (path MTU 4096), link-local only | |
| Engine | oMLX, branch above, Lightning MTP on | vLLM b12x build `eugr/spark-vllm-b12x:nightly-20261009` |
| Checkpoint | `Jundot/Qwen3.8-Flash-Next-oQ4e-mtp` (oQ 4-bit, 104 GB) | `local-inference-lab/Qwen3.8-Flash-Next-NVFP4`, rev `6c01ac37` |

The Mac ran driver 0.1.18 with three changes:
- [#17](https://github.com/ashhart/MCDMA/pull/17): accepts `26B5101f` after comparing its Apple interfaces;
- [#18](https://github.com/ashhart/MCDMA/pull/18): lets the link daemons meet over the LAN, since a Spark has no
  Thunderbolt networking;
- [#19](https://github.com/ashhart/MCDMA/pull/19): the link-flap fix; without it, the Mac's port stayed down after
  the Spark's port bounced, until a reboot.

The Thunderbolt cable is labelled TB4 and still negotiates 80 Gb/s with the Helios. It is not a bottleneck here.

## Recipe

**1. Bring the link up.** Follow [install](install.md) on the Mac and [Linux endpoints](linux-endpoints.md) on the
Spark. Both ends need:
- MAC-derived link-local GIDs;
- a static IPv6 neighbour for the other end on the CX ports (re-add them at boot);
- MTU 9000 on the Spark port.

Verify RDMA READ and WRITE with the checker before going on.

**2. Start the link daemons.** One link, `worker-a`, set up over the LAN as [link daemon](link-daemon.md)
describes:

```bash
# Spark: listen; exchange on the LAN port, pinned to the Mac's LAN link-local address
mcdma-rpcd listen worker-a rocep1s0f1 1 4096 <spark-lan-if>/<mac-lan-fe80>:18620 4 64

# Mac: connect; exchange on the LAN port, pinned to the Spark's LAN link-local address
mcdma-rpcd connect worker-a,en0/<spark-lan-fe80>,18620,rdma_mcrdma0,0,4096,4,64
```

`printf 'STATUS\n' | nc -U /tmp/mcdma-rpcd.sock` on the Mac should report `up`.

**3. Start the producer on the Spark.** Build `libmcdma-rpc.so` and mount this repository and the daemon's socket
into vLLM's container:

```bash
docker run -d --init --name flashnext-producer --gpus all --ipc=host --network host \
  --security-opt seccomp=unconfined --ulimit nofile=1048576:1048576 --ulimit memlock=-1 \
  -v $HOME/.cache/huggingface:/root/.cache/huggingface:ro -v $HOME/MCDMA:/mcdma:ro \
  -v /tmp/mcdma-rpcd.worker-a.sock:/tmp/mcdma-rpcd.worker-a.sock \
  -v $HOME/b12x-cache:/root/.cache/b12x -v $HOME/vllm-cache:/root/.cache/vllm -e B12X_COMPILE_WORKERS=2 \
  -e HF_HUB_OFFLINE=1 -e VLLM_PLE_TABLE_MEMORY=disk -e CUTE_DSL_ARCH=sm_121a -e SAFETENSORS_FAST_GPU=1 \
  -e VLLM_WORKER_MULTIPROC_METHOD=spawn -e VLLM_SSM_CONV_STATE_LAYOUT=DS -e VLLM_USE_AOT_COMPILE=1 \
  -e VLLM_USE_MEGA_AOT_ARTIFACT=1 -e VLLM_USE_V2_MODEL_RUNNER=1 -e B12X_POLICY_MODE=auto -e VLLM_MXFP8_LM_HEAD=1 \
  -e VLLM_B12X_MOE_FP4_LAYER_MAX_INPUT_SCALE=w13 -e PYTHONPATH=/mcdma/integrations/vllm \
  -e MCDMA_RPC_LIBRARY=/mcdma/build/rpc/libmcdma-rpc.so \
  eugr/spark-vllm-b12x:nightly-20261009 \
  vllm serve <snapshot of local-inference-lab/Qwen3.8-Flash-Next-NVFP4> --served-model-name Qwen3.8-Flash-Next \
    --host 0.0.0.0 --port 8100 --dtype bfloat16 --kv-cache-dtype auto --quantization modelopt_mixed \
    --load-format b12x --block-size 16 --max-model-len 65536 --max-num-seqs 1 --max-num-batched-tokens 8192 \
    --enable-chunked-prefill --no-enable-prefix-caching --recurrent-checkpoint-policy aligned \
    --gdn-decode-kernel b12x --linear-backend b12x --moe-backend b12x --no-enable-flashinfer-autotune \
    --limit-mm-per-prompt '{"image":0,"video":0}' --compilation-config '{"pass_config":{"fuse_act_quant":true}}' \
    --gpu-memory-utilization 0.70 \
    --speculative-config '{"method":"mtp","num_speculative_tokens":1}' \
    --kv-transfer-config '{"kv_connector":"MCDMAKVConnector","kv_connector_module_path":"mcdma_kv.connector","kv_role":"kv_producer","kv_connector_extra_config":{"links":["worker-a"]}}'
```

Why each choice:

- **bf16 KV:** handoffs refuse fp8 caches.
- **One sequence at a time:** the raw index keys are recorded by position.
- **No prefix caching:** each request keeps one private recurrent-state block until the Mac closes the handoff.
- **`--recurrent-checkpoint-policy aligned`:** this build's guard admits export-only connectors under that policy.
- **seccomp unconfined:** the disk-backed PLE table uses io_uring.
- **MTP speculative config:** the producer only ever returns one token. MTP runs so that the handoff carries the MTP
  head's cache.
- **NVFP4 checkpoint:** the GPTQ/AutoRound checkpoints did not load on the b12x linear path.

The first start compiles and tunes b12x kernels for 10 minutes or more. With the cache directories mounted, later
starts take about 2 minutes.

**4. Start the decoder on the Mac.**

```bash
export OMLX_REMOTE_PREFILL_URL=http://<spark>:8100
export OMLX_REMOTE_PREFILL_MODEL=Qwen3.8-Flash-Next              # the producer's served name
export OMLX_REMOTE_PREFILL_FOR=Qwen3.8-Flash-Next-oQ4e-mtp       # the local model it prefills for
export OMLX_REMOTE_PREFILL_LINKS=worker-a
export OMLX_REMOTE_PREFILL_CHECKSUM=0
export OMLX_MCDMA_RPC_LIBRARY=<MCDMA>/build/libmcdma-rpc.dylib
export OMLX_MCDMA_RPCD_SOCKET=/tmp/mcdma-rpcd.sock
taskpolicy -a omlx serve --model-dir <models> --host 0.0.0.0 --port 8100
```

Turn MTP on for the model (`"mtp_enabled": true` in its model settings).

If launchd starts oMLX, keep `taskpolicy -a`. A LaunchAgent otherwise runs it at the default scheduling role (priority
31), and decode dropped from 52–55 to 37 tok/s. `ProcessType Interactive` and `taskpolicy -t 0 -l 0` did not change
that. `-a`, the policies the system gives applications, did.

**5. Check that it works.**

For a prompt over the threshold the decoder logs `Remote prefill of N tokens, prefill … s, transfer … s`. Then
`MTP path activated … primed=N` should follow, with `N` the whole prompt. A `primed` of a few tokens means the head
started without the prompt: the producer does not run MTP, or its export left the head out (it logs why).

To check the state itself, put a passphrase in a long document and ask for it. Every run below found it.

## Results

All runs: greedy, thinking off, 512-token answers, the model loaded on both ends. The prompt is a long document with a
passphrase in it, followed by a request to quote the passphrase and write an 800-word essay. There are two runs per
row unless noted, and every run found the passphrase.

| Setup | 8k: first token s | 8k: decode tok/s | 8k: total s | 32k: first token s | 32k: decode tok/s | 32k: total s |
|---|---:|---:|---:|---:|---:|---:|
| Mac only, oMLX, no MTP | 8.8–9.2 | 41 | 21.1–21.6 | 37.2 | 47 | 48.0 |
| Spark only, vLLM as configured above, without MTP | 3.0 | 28 | 21.3 | 12.4 | 28 | 30.8 |
| Spark only, TensorFold, INT4, fp8 KV | 3.1 | 44 | 14.8 | 12.3 | 43 | 24.0–24.2 |
| Split, no MTP | 4.6 | 53–55 | 13.9–14.3 | 14.5 | 52 | 24.3 |
| Split, MTP, head starts without the prompt | 4.7–4.9 | 70–77 | 11.4–12.3 | | | |
| Split, MTP with the head's history from the Spark | 5.1–5.4 | 59–60 | 13.6–14.1 | 16.3 | 58 | 25.1 |

Notes:

- **The last row ran in a busier hour.** The Mac's screen was in use, and in that hour short prompts decoded at
  55–71 tok/s against 82 earlier, at the same tokens per draft step. On prose the head drafts about 2 tokens per step
  with or without the prompt history, so the history does not slow prose decode. The time of day does.
- **The 32k split rows are single runs.**

The prompt history pays where the answer copies from the prompt. The test: rewrite about 2,100–2,900 tokens of Python
with one variable renamed.

| Setup | Prompt tokens | Tokens per draft step | Acceptance | Decode tok/s |
|---|---:|---:|---:|---:|
| Mac only, head primed locally | 2,889 | 3.88 | 99.0 % | 118–121 |
| Split, head starts without the prompt | 2,889 | 2.0–2.5 | 72–85 % | 88 |
| Split, head history from the Spark | 2,100 / 2,308 | 3.88 | 98.4–98.7 % | 106 |

What the tables show:

- **Decode.** The split decodes faster than either machine alone: 52–77 tok/s on prose against TensorFold's 43 on
  one Spark, and more on copy-heavy answers. An 8k request with a 512-token answer finishes in 11.4–14.1 s, against
  14.8 s on TensorFold.
- **First token.** At 32k the totals are level. The Spark alone still gives the first token about 2–4 s sooner,
  because the split pays the handoff and the Mac's setup on top of the same prefill.
- **Mac alone loses.** Its prefill is about 3× slower.
- **Cost of the MTP head's history:**
  - the drafter adds about 10 % to the Spark's prefill: 2.96 → 3.28 s at 8k, 12.2 → 13.3 s at 32k;
  - 2.3 KB more per token over the link (336 → 355 MB at 8k, 993 → 1,066 MB at 32k);
  - one hidden state.

## Where the first token's time goes (8k, MTP)

| Stage | Seconds |
|---|---:|
| Prefill on the Spark, MTP drafter included | 3.25–3.31 |
| Pull of 355 MB over the link | 0.32 |
| The rest: request setup, building caches, the generation prompt's few tokens, first decode step | about 1.5–1.8 |

The pull ran at 7.6–9.4 Gbit/s in every handoff, far below the 38 Gbit/s the Qwen3-4B note reports for a single
pre-registered region. This path moves frames through `mcdma-rpcd`'s mailbox (a 4 MiB request half and a 64 MiB
reply half here) and copies each entry into MLX arrays. Larger mailboxes, more links, and pulling straight into
preallocated arrays have not been tried. Neither cable limits it.

## Pitfalls we hit

- **The connector exported before the drafter ran.** vLLM's V1 model runner calls `wait_for_save` before the MTP
  drafter writes its cache. With MTP on, the connector exports in `get_finished`, which comes after the drafter in
  both runners.
- **Speculative decoding widens the conv windows.** The Gated DeltaNet and PLE conv windows get one more slot per
  speculative token, and prefill fills the first slots. The export takes those and refuses any other width.
- **The MTP head's last row is not prompt history.** It pairs the last prompt token with vLLM's own sampled token.
  It is left out, and the decoder pairs the hidden state of that token with its own first token.
- **Kernel tuning can run the Spark out of memory.** b12x tunes kernels after vLLM has taken its share of the GB10's
  unified memory, and with four compile workers and the default memory share, available memory fell below 5 GiB.
  On this machine that risks a hang, so a watchdog killed the container. Two workers, 70 % GPU memory and a persisted
  tuning cache fixed it. That leaves a 3.6 GiB KV cache, 122k tokens: enough for one 65k-token request at a time,
  which is all the producer runs.
- **Ad-hoc kexts and macOS updates.** Every macOS update changes the build ID, and the driver refuses to start until
  the new build is compared and allowlisted (`tools/compare-apple-build.py`).

## Limitations

- **One request at a time on the Spark.** The raw index keys are recorded by position for the one request being
  prefilled. Concurrent prompts over the threshold queue on the Mac.
- **Single-rank producer, one link, text only.** Prompts with images prefill on the Mac.
- **One pair of machines, one week.** Decode rates on the Mac move with whatever else uses its GPU, so compare rows
  measured together. The copy test used different slices of one file for the split-with-history runs, because the
  Mac's prefix cache already held the first slice.
- **Experimental code.** The oMLX side is a fork branch on top of an unmerged one. The vLLM side depends on internals
  of one vLLM build: the QSA run, the speculator's `propose`, and conv-state layouts.
- **Security.** The Mac runs with SIP off and Reduced Security to load the ad-hoc kext.
