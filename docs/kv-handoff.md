# KV handoff over MCDMA

A decoder on the Mac can have a CUDA machine prefill long prompts and then pull the resulting KV cache over an MCDMA
link. The producer is vLLM with the connector in `integrations/vllm/mcdma_kv`; oMLX's remote prefill is a decoder
that speaks this protocol. Both ends use `mcdma-rpcd` links, so neither application opens a verbs context.

Status: offline-tested with this repository's checks and against oMLX's decoder over a stand-in link. It has not yet
run on hardware or inside vLLM.

## Setting up vLLM

1. Install `mcdma-rpcd` and `libmcdma-rpc` on the vLLM host (see [link daemon](link-daemon.md)) and start one listen
   daemon per tensor-parallel rank, owned by the user vLLM runs as. Give each link a reply half large enough for
   several page rows of one layer; 64 MiB suits most models. The daemons set each link up over a Thunderbolt or USB4
   network cable between the two hosts, so the vLLM host needs one to the Mac as well as the RDMA link.
2. Put `integrations/vllm` on vLLM's `PYTHONPATH`, or copy `mcdma_kv` into its environment.
3. Start vLLM with the connector as a producer, naming the links in rank order:

```bash
vllm serve MODEL --kv-transfer-config '{"kv_connector": "MCDMAKVConnector",
  "kv_connector_module_path": "mcdma_kv.connector", "kv_role": "kv_producer",
  "kv_connector_extra_config": {"links": ["worker-a"]}}'
```

On the Mac, the connect daemon needs a peer entry for each of those links.

Extra settings: `export_ttl_s` (default 120) frees a handoff the decoder never closed, and `max_layers` exports only
layers below that index, for models whose drafter layers share the cache.

## What a request does

A decoder sends an ordinary completion request with the prompt's token IDs and

```json
"kv_transfer_params": {"mcdma_handoff": {"id": "<32 hex digits>", "export_from": 0}}
```

The connector limits the request to one generated token. When the step that finishes the prompt ends, each rank
records an export of its pages from `export_from` on, rounded down to a page row, and vLLM keeps the request's blocks
allocated. The rank's serving thread then answers the decoder over its link. Blocks are released when the decoder
closes the handoff, or after `export_ttl_s`.

Supported caches: full attention and MLA layers in bf16, fp16 or fp32, and the hybrid layers of Qwen3.8 Flash Next
(below). Sliding-window, other Mamba-type and fp8 caches are refused with an explanation the decoder receives.

## Hybrid models

Qwen3.8 Flash Next mixes 36 Gated DeltaNet layers with 12 QSA sparse-attention layers, and carries a PLE short-conv
state on one layer. A recurrent state exists only for a whole prefix, so a hybrid handoff always covers prompt tokens
`[0, T)`: ask with `export_from` 0. The manifest's `layers` are then named entries rather than page arrays, each with
`kind`, the model `layer`, a `name`, and a fixed layout that does not depend on vLLM's own:

| kind | name | shape | dims | contents |
| --- | --- | --- | --- | --- |
| `qsa` | `k`, `v` | `[T, kv_heads, head_dim]` | `block, head, head_dim` | keys post-RoPE and values, as cached |
| `qsa` | `index_keys` | `[T, indexer_head_dim]` | `block, index_dim` | raw index keys, pre-norm and pre-RoPE |
| `gdn` | `conv` | `[conv_kernel - 1, conv_dim]` | `block, channel` | conv state, oldest tap first, channels q, k, v |
| `gdn` | `ssm` | `[v_heads, v_dim, k_dim]` | `block, v_dim, k_dim` | recurrent state, float32 |
| `ple` | `conv` | `[(kernel - 1) * dilation, hc_dim]` | `block, channel` | PLE short-conv state, oldest tap first |
| `mtp` | `k`, `v` | `[T - 1, kv_heads, head_dim]` | `block, head, head_dim` | the MTP head's QSA layer, as for `qsa` |
| `mtp` | `index_keys` | `[T - 1, indexer_head_dim]` | `block, index_dim` | its raw index keys |
| `mtp` | `hidden` | `[1, hc_count * hidden_size]` | `block, hidden` | target hidden state of token `T - 1`, all streams |

`first_token` is 0 and `block_size` 1. A DATA frame's `layer` field is the entry's `index`, and its rows run along the
entry's first dimension. Text-only prompts index positions `0 .. T-1`, and the PLE layer's n-gram context is the
prompt's last tokens, so the decoder rebuilds both.

vLLM keeps raw index keys only for a QSA layer's last four tokens, so the connector wraps the layer's run and copies
each prefill chunk's keys out by position. That needs one request in flight at a time, and the hybrid KV cache
manager. Run the producer on a vLLM build with Flash Next support (the b12x Spark build was used) with:

```bash
--max-num-seqs 1 --no-enable-prefix-caching --kv-cache-dtype auto --recurrent-checkpoint-policy aligned
```

With `--no-enable-prefix-caching` each request keeps one private recurrent-state block, which the connector holds
until the decoder closes the handoff. Every entry is copied into private tensors on the step that finishes the
prefill, about 115 MB of fixed state plus 27 KB per prompt token.

The `mtp` entries let a decoder that drafts with the model's MTP head start with the whole prompt in the head's
cache. They exist when the producer also runs MTP speculative decoding,
`--speculative-config '{"method":"mtp","num_speculative_tokens":1}'`: its drafter caches one QSA layer (model layer
`num_hidden_layers`) whose row `t` pairs the target's hidden state at `t` with prompt token `t + 1`. Rows `[0, T - 1)`
are prompt history; row `T - 1` pairs with vLLM's own sampled token and is left out, and the hidden state of token
`T - 1` comes instead, for the decoder to pair with its own next token. The connector records that hidden state
from the speculator's `propose` call, and with MTP on it exports in `get_finished`, which follows the drafter in both
model runners (V1 calls `wait_for_save` before it). Speculative decoding also widens each Gated DeltaNet and PLE conv
window by one slot per speculative token; prefill fills the first slots, and the export carries only those. About
2.3 KB per prompt token more.

## Protocol 1

Every message starts with a 128-byte little-endian header:

| Bytes | Field |
| --- | --- |
| 0-3 | Magic `MKVH` |
| 4-5 | Version, 1 |
| 6-7 | Kind |
| 8-23 | Handoff ID |
| 24-27 | Frame index |
| 28-31 | Frame count |
| 32-35 | Layer index |
| 36-39 | Flags; bit 0 means `crc` is set |
| 40-47 | First page row of the frame |
| 48-55 | Page rows in the frame |
| 56-63 | Payload bytes after the header |
| 64-67 | zlib CRC-32 of the payload |

The decoder is the link's client and asks; the producer is its service and answers:

| Request | Answers |
| --- | --- |
| `OPEN` (1), payload `{"checksum": true}` | `MANIFEST` (2) with the manifest as JSON; `WAIT` (3) when the export is not recorded yet; `ERROR` (4) with a reason |
| `PULL` (5) for frame N | `DATA` (6): one frame of page rows; `ERROR` |
| `CLOSE` (7) | `ACK` (8); the producer frees the pages |

The manifest describes one rank's export:

```json
{"protocol": 1, "handoff": "<hex>", "model": "org/model", "prompt_tokens": 18000, "first_token": 16384,
 "token_sha256": "<hex>", "block_size": 16, "tp_rank": 0, "tp_size": 2, "frames": 120,
 "layers": [{"index": 0, "kind": "attention", "shape": [101, 4, 16, 256],
             "dims": ["block", "head", "token", "kv_head_dim"], "dtype": "bfloat16",
             "heads": 4, "total_heads": 8, "head_size": 128}]}
```

- `token_sha256` is the SHA-256 of the prompt's token IDs as little-endian uint32 values, so the decoder can refuse
  pages computed for a different prompt.
- `block_size` is tokens per page row, and `first_token` is the prompt position of the first exported row.
- `shape` is the layer's exported page array: the cache tensor's shape with its block dimension cut to the exported
  rows. `dims` names each dimension: `block`, `token`, `head`, `head_dim`, `kv` (keys, then values), `kv_head_dim`
  (keys and values side by side) or `latent` (MLA rows of `latent_size` latent values, then `rope_size` rope values).
  The connector learns the labels from vLLM's own attention backend, so they follow the backend in use.
- A `DATA` payload is rows `[first, first + rows)` of one layer's page array, C-contiguous in the manifest's shape.
- `heads` are this rank's key/value heads out of `total_heads`. When there are more ranks than heads, each head is
  repeated on consecutive ranks.

Each link carries one call at a time: the decoder waits for an answer before sending the next request.
