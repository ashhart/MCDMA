"""Hybrid handoffs for Qwen3.8 Flash Next: whole-prefix state of its QSA, Gated DeltaNet and PLE layers.

A hybrid model's recurrent state is only defined for a whole prefix, so a hybrid handoff always
covers prompt tokens [0, T) and exports each layer's state at T as named entries in a fixed layout,
whatever vLLM's own layout is (docs/kv-handoff.md, "Hybrid models"):

    qsa  k, v        [T, kv_heads, head_dim]           post-RoPE, as cached
    qsa  index_keys  [T, indexer_head_dim]             raw: pre-norm, pre-RoPE
    gdn  conv        [conv_kernel - 1, conv_dim]       oldest tap first; channels q | k | v
    gdn  ssm         [v_heads, v_dim, k_dim]           float32
    ple  conv        [(kernel - 1) * dilation, hc_dim] oldest tap first
    mtp  k, v        [T - 1, kv_heads, head_dim]       the MTP head's QSA layer, as for qsa
    mtp  index_keys  [T - 1, indexer_head_dim]
    mtp  hidden      [1, hc_count * hidden_size]       target hidden of token T - 1, all streams

The mtp entries exist only when the producer runs MTP speculative decoding (layer index
num_hidden_layers). Its row t pairs the target's hidden state at t with prompt token t + 1, so
rows [0, T - 1) are prompt history; row T - 1 pairs with vLLM's own sampled token and is left
out. The decoder folds the hidden state of token T - 1 with its own next token.

vLLM keeps no raw index keys beyond the last four tokens, so `install_index_key_hook` wraps the
QSA layer's run and copies each prefill chunk's raw keys out by position. That needs one request
in flight at a time: run the producer with --max-num-seqs 1. Every entry is snapshotted into
private tensors on the step that finishes the prefill, so serving never reads live caches.
"""

from __future__ import annotations

import logging
import re
from dataclasses import dataclass
from typing import Any

import torch

from .export import LayerPages

# Under vLLM's logger, so the server's log configuration shows these lines.
logger = logging.getLogger("vllm.mcdma_kv.hybrid")
_QSA = re.compile(r"layers\.(\d+)\.self_attn\.attn$")
_GDN = re.compile(r"layers\.(\d+)\.linear_attn$")
_PLE = re.compile(r"layers\.(\d+)\.ple$")
_DIMS = {
    ("qsa", "k"): ["block", "head", "head_dim"],
    ("qsa", "v"): ["block", "head", "head_dim"],
    ("qsa", "index_keys"): ["block", "index_dim"],
    ("gdn", "conv"): ["block", "channel"],
    ("gdn", "ssm"): ["block", "v_dim", "k_dim"],
    ("ple", "conv"): ["block", "channel"],
    ("mtp", "k"): ["block", "head", "head_dim"],
    ("mtp", "v"): ["block", "head", "head_dim"],
    ("mtp", "index_keys"): ["block", "index_dim"],
    ("mtp", "hidden"): ["block", "hidden"],
}


def is_hybrid(specs: list[Any]) -> bool:
    return any(type(spec).__name__ == "MambaSpec" for spec in specs)


@dataclass
class HybridPages(LayerPages):
    """One canonical entry: a private tensor whose first dimension is the row axis frames cut."""

    kind: str = ""
    layer: int = -1
    name: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {"index": self.index, "kind": self.kind, "layer": self.layer, "name": self.name,
                "shape": self.shape, "dims": self.dims, "dtype": self.dtype}


class IndexKeyRecorder:
    """Raw QSA index keys by position, per layer, for the one request being prefilled."""

    def __init__(self, max_tokens: int) -> None:
        self.max_tokens = max_tokens
        self._buffers: dict[int, torch.Tensor] = {}
        self._unknown: set[str] = set()

    def record(self, module: Any, positions: torch.Tensor, raw_keys: torch.Tensor, rows: int) -> None:
        if rows <= 0 or (raw_keys.is_cuda and torch.cuda.is_current_stream_capturing()):
            return
        name = str(getattr(module, "layer_name", ""))
        found = _QSA.search(name)
        if found is None:
            if name not in self._unknown:
                self._unknown.add(name)
                logger.warning("MCDMA handoff: QSA layer %r has no layer index; its index keys are not recorded", name)
            return
        layer = int(found.group(1))
        buffer = self._buffers.get(layer)
        if buffer is None:
            buffer = torch.zeros(self.max_tokens, raw_keys.shape[-1], dtype=raw_keys.dtype,
                                 device=raw_keys.device)
            self._buffers[layer] = buffer
            logger.info("MCDMA handoff: recording raw index keys of layer %d (%s, positions %s, keys %s %s)",
                        layer, name, tuple(positions.shape), tuple(raw_keys.shape), raw_keys.dtype)
        logical = positions[:rows] if positions.dim() == 1 else positions[0, :rows]
        buffer.index_copy_(0, logical.to(torch.long), raw_keys[:rows])

    def has(self, layer: int) -> bool:
        return layer in self._buffers

    def keys(self, layer: int, tokens: int) -> torch.Tensor:
        buffer = self._buffers.get(layer)
        if buffer is None:
            raise ValueError(f"no raw index keys were recorded for layer {layer}")
        return buffer[:tokens].clone()


class HiddenRecorder:
    """The target hidden state the MTP drafter received last, per step, with its position."""

    keep = 8

    def __init__(self) -> None:
        self._rows: list[tuple[torch.Tensor, torch.Tensor]] = []

    def record(self, positions: torch.Tensor, hidden: torch.Tensor, tokens: int) -> None:
        if tokens <= 0 or (hidden.is_cuda and torch.cuda.is_current_stream_capturing()):
            return
        if not self._rows:
            logger.info("MCDMA handoff: MTP target hidden states %s %s, positions %s",
                        tuple(hidden.shape), hidden.dtype, tuple(positions.shape))
        position = positions[..., tokens - 1]
        if position.dim():  # M-RoPE: [3, tokens]; a text token's axes agree
            position = position[0]
        # Clones only: comparing positions here would sync the GPU on every step.
        self._rows.append((position.clone(), hidden[tokens - 1 : tokens].clone()))
        del self._rows[: -self.keep]

    def hidden(self, position: int) -> torch.Tensor | None:
        for found, row in reversed(self._rows):
            if int(found.item()) == position:
                return row
        return None


def install_hidden_hook(recorder: HiddenRecorder) -> None:
    """Wrap vLLM's MTP speculator so the target hidden state of each step's last token reaches `recorder`."""
    import inspect

    from vllm.v1.worker.gpu.spec_decode.autoregressive.speculator import AutoRegressiveSpeculator

    original = AutoRegressiveSpeculator.propose
    if getattr(original, "_mcdma_recorder", None) is not None:
        original._mcdma_recorder = recorder
        return
    signature = inspect.signature(original)

    def propose(self, *args, **kwargs):
        bound = signature.bind(self, *args, **kwargs)
        batch, hidden = bound.arguments["input_batch"], bound.arguments["last_hidden_states"]
        if not bound.arguments.get("dummy_run") and not bound.arguments.get("is_profile"):
            propose._mcdma_recorder.record(batch.positions, hidden, int(batch.num_tokens))
        return original(self, *args, **kwargs)

    propose._mcdma_recorder = recorder
    AutoRegressiveSpeculator.propose = propose
    logger.info("MCDMA handoff: recording the MTP drafter's target hidden states for hybrid exports")


def install_index_key_hook(recorder: IndexKeyRecorder) -> None:
    """Wrap the b12x QSA run so every prefill chunk's raw index keys reach `recorder`."""
    from vllm.models.qwen4_exp.nvidia import b12x_qsa

    cls = b12x_qsa.Qwen4ExpQSAAttention
    original = cls._run_b12x_qsa
    if getattr(original, "_mcdma_recorder", None) is not None:
        original._mcdma_recorder = recorder
        return

    def run(self, *, positions, raw_index_key, rows, **kwargs):
        run._mcdma_recorder.record(self, positions, raw_index_key, int(rows))
        return original(self, positions=positions, raw_index_key=raw_index_key, rows=rows, **kwargs)

    run._mcdma_recorder = recorder
    cls._run_b12x_qsa = run
    logger.info("MCDMA handoff: recording raw QSA index keys for hybrid exports")


def _state_parts(tensor: torch.Tensor, slot: int, spec: Any) -> list[torch.Tensor]:
    """A Mamba-type state page split into its states, as vLLM's bind_kv_cache lays them out."""
    page = tensor[slot].reshape(-1).view(torch.uint8)
    parts, offset = [], 0
    for shape, dtype in zip(spec.shapes, spec.dtypes):
        count = 1
        for size in shape:
            count *= int(size)
        nbytes = count * torch.empty((), dtype=dtype).element_size()
        parts.append(page[offset:offset + nbytes].view(dtype).reshape(tuple(int(s) for s in shape)))
        offset += nbytes
    return parts


def _channel_major(state: torch.Tensor, taps: int, spec_tokens: int = 0) -> torch.Tensor:
    """A conv state as [taps, channels], whichever of vLLM's two layouts holds it.

    With speculative decoding vLLM widens each window by one slot per speculative token. Prefill
    (causal_conv1d_fn for Gated DeltaNet, ple_conv's writeback for PLE) fills the first `taps`
    slots, oldest first; the rest only hold speculative tokens."""
    width = taps + spec_tokens
    if state.shape[-1] == width and state.shape[0] != width:
        state = state.transpose(0, 1)
    if state.shape[0] != width:
        raise ValueError(f"conv state {tuple(state.shape)} has no {width}-slot window")
    return state[:taps]


def export_entries(caches: dict[str, torch.Tensor], groups: dict[str, int], specs: list[Any],
                   block_ids: tuple[tuple[int, ...], ...], tokens: int, recorder: IndexKeyRecorder,
                   config: Any, hidden: HiddenRecorder | None = None, spec_tokens: int = 0) -> list[HybridPages]:
    """Snapshot every canonical entry of one request's prefix [0, tokens)."""
    conv_taps = int(config.linear_conv_kernel_dim) - 1
    ple_taps = (int(config.ple_conv_kernel_size) - 1) * int(config.ngram_size)
    backbone = int(config.num_hidden_layers)
    found: list[tuple[int, str, str, torch.Tensor]] = []
    mtp: list[tuple[int, str, str, torch.Tensor]] = []
    for name, tensor in caches.items():
        group = groups.get(name)
        if group is None:
            continue
        spec, ids = specs[group], block_ids[group]
        if (match := _QSA.search(name)) is not None:
            layer = int(match.group(1))
            heads, head_size = int(spec.num_kv_heads), int(spec.head_size)
            # [num_blocks, 2 (K, V), block_size, heads * head_size]
            blocks = torch.tensor(ids[: -(-tokens // spec.block_size)], device=tensor.device)
            # The MTP head's layer follows the backbone; its last row is not prompt history.
            kind, rows_kept, into = ("qsa", tokens, found) if layer < backbone else ("mtp", tokens - 1, mtp)
            if kind == "mtp" and not recorder.has(layer):
                logger.warning("MCDMA handoff: no raw index keys for MTP layer %d; leaving the MTP head out", layer)
                continue
            pages = tensor.index_select(0, blocks)
            for part, label in ((0, "k"), (1, "v")):
                rows = pages[:, part].reshape(-1, heads, head_size)[:rows_kept]
                into.append((layer, kind, label, rows.clone()))
            into.append((layer, kind, "index_keys", recorder.keys(layer, tokens)[:rows_kept]))
        elif (match := _GDN.search(name)) is not None:
            conv, ssm = _state_parts(tensor, ids[0], spec)[:2]
            found.append((int(match.group(1)), "gdn", "conv", _channel_major(conv, conv_taps, spec_tokens).clone()))
            found.append((int(match.group(1)), "gdn", "ssm", ssm.to(torch.float32).clone()))
        elif (match := _PLE.search(name)) is not None:
            conv = _state_parts(tensor, ids[0], spec)[0]
            found.append((int(match.group(1)), "ple", "conv", _channel_major(conv, ple_taps, spec_tokens).clone()))
    if mtp:
        row = hidden.hidden(tokens - 1) if hidden is not None and tokens > 1 else None
        if row is None:
            logger.warning("MCDMA handoff: no MTP hidden state for token %d; the decoder primes its MTP head itself",
                           tokens - 1)
        else:
            found += mtp + [(mtp[0][0], "mtp", "hidden", row)]
    entries = []
    for position, (layer, kind, label, tensor) in enumerate(sorted(found, key=lambda e: (e[0], e[1], e[2]))):
        entries.append(HybridPages(
            index=position, tensor=tensor.contiguous(), block_dim=0, rows=list(range(tensor.shape[0])),
            dims=_DIMS[(kind, label)], dtype=str(tensor.dtype).removeprefix("torch."),
            kind=kind, layer=layer, name=label))
    return entries
