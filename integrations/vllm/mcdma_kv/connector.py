"""vLLM KV connector that exports a finished prefill's pages over an mcdma-rpcd link.

Enable it with:
    --kv-transfer-config '{"kv_connector": "MCDMAKVConnector", "kv_connector_module_path": "mcdma_kv.connector",
                           "kv_role": "kv_producer", "kv_connector_extra_config": {"links": ["worker-a"]}}'
A request asks for a handoff with kv_transfer_params {"mcdma_handoff": {"id": "<32 hex digits>", "export_from": N}}.
It generates one token, and its blocks stay allocated until the decoder closes the handoff or it expires.
Each tensor-parallel rank serves its own heads on links[rank]. docs/kv-handoff.md describes the protocol.
"""

from __future__ import annotations

import logging
import math
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import torch
from vllm.distributed.kv_transfer.kv_connector.v1.base import (
    KVConnectorBase_V1,
    KVConnectorMetadata,
    KVConnectorRole,
)

try:  # vLLM builds with the hybrid KV cache manager; hybrid models need it.
    from vllm.distributed.kv_transfer.kv_connector.v1.base import SupportsHMA
except ImportError:  # pragma: no cover - older vLLM
    class SupportsHMA:  # type: ignore[no-redef]
        pass

from . import hybrid
from .export import DTYPES, Export, LayerPages, geometry
from .mailbox import MailboxError, ServiceMailbox
from .responder import ExportTable, Responder

logger = logging.getLogger("vllm.mcdma_kv.connector")
_LAYER = re.compile(r"layers\.(\d+)\.")
# How long the serving thread waits before attaching again to a daemon that is not up.
_REATTACH_S = 5.0


@dataclass
class HandoffRequest:
    """A request whose prompt is being prefilled for a handoff."""

    request_id: str
    handoff: bytes
    tokens: list[int]
    export_from: int
    block_ids: tuple[tuple[int, ...], ...] = ()
    failed: str = ""


@dataclass
class MCDMAHandoffMetadata(KVConnectorMetadata):
    """Handoffs whose prompts finished prefilling in this step."""

    ready: list[HandoffRequest] = field(default_factory=list)


def _tensor_parallel() -> tuple[int, int]:
    try:
        from vllm.distributed import get_tensor_model_parallel_rank, get_tensor_model_parallel_world_size

        return get_tensor_model_parallel_rank(), get_tensor_model_parallel_world_size()
    except Exception:
        return 0, 1


class MCDMAKVConnector(KVConnectorBase_V1, SupportsHMA):
    """Producer side of the MCDMA KV handoff."""

    def __init__(self, vllm_config: Any, role: KVConnectorRole, kv_cache_config: Any = None) -> None:
        super().__init__(vllm_config, role, kv_cache_config)
        extra = self._kv_transfer_config
        self._links = [str(name) for name in (extra.get_from_extra_config("links", []) or [])]
        self._max_layers = int(extra.get_from_extra_config("max_layers", 0) or 0)
        ttl_s = float(extra.get_from_extra_config("export_ttl_s", 120))
        model_config = vllm_config.model_config
        served = getattr(model_config, "served_model_name", None) or model_config.model
        self._model = served[0] if isinstance(served, (list, tuple)) else str(served)
        self._total_heads = int(model_config.get_total_num_kv_heads())
        self._latent = int(getattr(model_config.hf_text_config, "kv_lora_rank", 0) or 0)
        self._groups = {name: index for index, group in enumerate(kv_cache_config.kv_cache_groups)
                        for name in group.layer_names}
        self._specs = [group.kv_cache_spec for group in kv_cache_config.kv_cache_groups]
        self._num_blocks = int(kv_cache_config.num_blocks)
        # Hybrid models (Gated DeltaNet + QSA, Qwen3.8 Flash Next) export whole-prefix state.
        self._hybrid = hybrid.is_hybrid(self._specs)
        self._text_config = model_config.hf_text_config
        self._recorder = hybrid.IndexKeyRecorder(int(model_config.max_model_len)) if self._hybrid else None
        # With MTP speculative decoding the drafter's cache is prompt history the decoder's head can reuse.
        speculative = getattr(vllm_config, "speculative_config", None)
        self._hidden = (hybrid.HiddenRecorder()
                        if self._hybrid and speculative is not None and "mtp" in str(speculative.method) else None)
        self._spec = int(getattr(speculative, "num_speculative_tokens", 0) or 0) if speculative is not None else 0
        # Exports held until get_finished, by when the step's MTP drafter has written its cache.
        self._deferred: list[HandoffRequest] = []
        # Scheduler side: requests being prefilled, and requests whose blocks the handoff holds.
        self._tracked: dict[str, HandoffRequest] = {}
        self._holding: set[str] = set()
        # Worker side.
        self._caches: dict[str, torch.Tensor] = {}
        self._table = ExportTable(ttl_s=ttl_s)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._stream: torch.cuda.Stream | None = None

    @classmethod
    def supports_aligned_hybrid_transfer(cls, config: Any) -> bool:
        """vLLM builds that guard QSA models accept export-only producers under aligned retention.

        That guard protects connectors that import recurrent state into vLLM. This one never
        imports: it copies each handoff's state out at the end of its prefill, so the retention
        policy (and, with prefix caching off, no retention at all) cannot affect what it exports.
        """
        transfer = getattr(config, "kv_transfer_config", None)
        return getattr(transfer, "kv_role", None) == "kv_producer"

    # Scheduler side -------------------------------------------------------

    def on_new_request(self, request: Any) -> None:
        params = (getattr(request, "kv_transfer_params", None) or {}).get("mcdma_handoff")
        if not params:
            return
        tokens = list(request.prompt_token_ids or [])
        try:
            handoff = bytes.fromhex(str(params["id"]))
            export_from = int(params.get("export_from", 0) or 0)
        except (KeyError, TypeError, ValueError):
            logger.warning("MCDMA handoff request %s has a malformed id", request.request_id)
            return
        if len(handoff) != 16:
            logger.warning("MCDMA handoff request %s has a malformed id", request.request_id)
            return
        tracked = HandoffRequest(request.request_id, handoff, tokens, export_from)
        if not 0 <= export_from < len(tokens):
            tracked.failed = f"export_from {export_from} is outside the prompt's {len(tokens)} tokens"
        self._tracked[request.request_id] = tracked
        request.max_tokens = 1
        if getattr(request, "sampling_params", None) is not None:
            request.sampling_params.max_tokens = 1

    def get_num_new_matched_tokens(self, request: Any, num_computed_tokens: int) -> tuple[int, bool]:
        return 0, False

    def update_state_after_alloc(self, request: Any, blocks: Any, num_external_tokens: int) -> None:
        return

    def build_connector_meta(self, scheduler_output: Any) -> KVConnectorMetadata:
        ready: list[HandoffRequest] = []

        def advance(request_id: str, blocks: Any, computed: int, replace: bool) -> None:
            tracked = self._tracked.get(request_id)
            if tracked is None:
                return
            if blocks is not None:
                if replace or not tracked.block_ids:
                    tracked.block_ids = tuple(tuple(int(b) for b in group) for group in blocks)
                else:
                    tracked.block_ids = tuple(
                        (*old, *(int(b) for b in new)) for old, new in zip(tracked.block_ids, blocks))
            scheduled = scheduler_output.num_scheduled_tokens.get(request_id, 0)
            if computed + scheduled >= len(tracked.tokens):
                ready.append(self._tracked.pop(request_id))
                self._holding.add(request_id)

        for new in scheduler_output.scheduled_new_reqs:
            advance(new.req_id, new.block_ids, new.num_computed_tokens, True)
        cached = scheduler_output.scheduled_cached_reqs
        resumed = set(getattr(cached, "resumed_req_ids", ()) or ())
        for index, request_id in enumerate(cached.req_ids):
            advance(request_id, cached.new_block_ids[index], cached.num_computed_tokens[index],
                    request_id in resumed)
        return MCDMAHandoffMetadata(ready=ready)

    def request_finished(self, request: Any, block_ids: list[int]) -> tuple[bool, dict[str, Any] | None]:
        self._tracked.pop(request.request_id, None)
        # A handoff's blocks stay allocated until the worker reports it closed.
        return request.request_id in self._holding, None

    def request_finished_all_groups(self, request: Any,
                                    block_ids: tuple[list[int], ...]) -> tuple[bool, dict[str, Any] | None]:
        return self.request_finished(request, [])

    def update_connector_output(self, connector_output: Any) -> None:
        for request_id in getattr(connector_output, "finished_sending", None) or ():
            self._holding.discard(request_id)

    def has_pending_push_work(self) -> bool:
        return bool(self._holding)

    # Worker side ----------------------------------------------------------

    def register_kv_caches(self, kv_caches: dict[str, torch.Tensor]) -> None:
        self._caches = kv_caches
        if self._hybrid:
            hybrid.install_index_key_hook(self._recorder)
            if self._hidden is not None:
                hybrid.install_hidden_hook(self._hidden)
            for name, tensor in kv_caches.items():
                group = self._groups.get(name)
                spec = type(self._specs[group]).__name__ if group is not None else "-"
                logger.debug("MCDMA handoff cache %s %s %s %s", name, spec, tuple(tensor.shape), tensor.dtype)
        rank, _ = _tensor_parallel()
        if rank >= len(self._links):
            logger.warning("MCDMA handoff: no link for tensor-parallel rank %d", rank)
            return
        self._stream = torch.cuda.Stream()
        self._thread = threading.Thread(target=self._serve, args=(self._links[rank],), name="mcdma-kv",
                                        daemon=True)
        self._thread.start()

    def start_load_kv(self, forward_context: Any, **kwargs: Any) -> None:
        return

    def wait_for_layer_load(self, layer_name: str) -> None:
        return

    def save_kv_layer(self, layer_name: str, kv_layer: torch.Tensor, attn_metadata: Any, **kwargs: Any) -> None:
        return

    def wait_for_save(self) -> None:
        metadata = self._get_connector_metadata()
        if not isinstance(metadata, MCDMAHandoffMetadata) or not metadata.ready:
            return
        if self._hidden is not None:
            # V1's runner calls wait_for_save before the MTP drafter runs; get_finished follows it in V1 and V2.
            self._deferred.extend(metadata.ready)
            return
        self._serve_ready(metadata.ready)

    def _serve_ready(self, requests: list[HandoffRequest]) -> None:
        # Serving reads wait on this event, so they see the pages this step wrote.
        ready = torch.cuda.Event()
        ready.record()
        for request in requests:
            try:
                if request.failed:
                    raise ValueError(request.failed)
                export = self._export(request, ready)
            except Exception as exc:
                logger.warning("MCDMA handoff %s cannot be served: %s", request.handoff.hex(), exc)
                self._table.add(request.handoff, request.request_id, str(exc))
            else:
                self._table.add(request.handoff, request.request_id, export)

    def get_finished(self, finished_req_ids: set[str]) -> tuple[set[str] | None, set[str] | None]:
        if self._deferred:
            requests, self._deferred = self._deferred, []
            self._serve_ready(requests)
        return self._table.take_finished() or None, None

    def shutdown(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def _export(self, request: HandoffRequest, ready: Any) -> Export:
        if self._hybrid:
            if request.export_from:
                raise ValueError("hybrid handoffs export the whole prefix; ask with export_from 0")
            entries = hybrid.export_entries(self._caches, self._groups, self._specs, request.block_ids,
                                            len(request.tokens), self._recorder, self._text_config, self._hidden,
                                            self._spec)
            # The snapshots were taken on this stream; serving waits for them.
            done = torch.cuda.Event()
            done.record()
            return Export(request.request_id, request.handoff, request.tokens, 0, 1, entries, ready=done)
        layers, per_row = [], None
        for name, tensor in self._caches.items():
            found = _LAYER.search(name)
            group = self._groups.get(name)
            if found is None or group is None or (self._max_layers and int(found.group(1)) >= self._max_layers):
                continue
            spec = self._specs[group]
            mla = "MLA" in type(spec).__name__
            if type(spec).__name__ not in {"FullAttentionSpec", "MLAAttentionSpec"}:
                raise ValueError(f"{type(spec).__name__} layers cannot be exported")
            dtype = str(tensor.dtype).removeprefix("torch.")
            if dtype not in DTYPES:
                raise ValueError(f"{dtype} KV caches cannot be exported")
            labels, ratio = geometry(tuple(tensor.shape), num_blocks=self._num_blocks, block_size=spec.block_size,
                                     heads=spec.num_kv_heads, head_size=spec.head_size, mla=mla,
                                     probe=self._probe(name))
            tokens_per_row = spec.block_size // ratio
            if per_row not in (None, tokens_per_row):
                raise ValueError("layers hold different numbers of tokens per page row")
            per_row = tokens_per_row
            rows = [block * ratio + part for block in request.block_ids[group] for part in range(ratio)]
            first, last = request.export_from // per_row, math.ceil(len(request.tokens) / per_row)
            layers.append(LayerPages(
                index=int(found.group(1)), tensor=tensor, block_dim=labels.index("block"), rows=rows[first:last],
                dims=labels, dtype=dtype, heads=spec.num_kv_heads, total_heads=self._total_heads,
                head_size=spec.head_size, latent_size=self._latent if mla else 0,
                rope_size=spec.head_size - self._latent if mla else 0))
        if not layers or per_row is None:
            raise ValueError("the model has no exportable attention layers")
        layers.sort(key=lambda layer: layer.index)
        first_token = request.export_from // per_row * per_row
        return Export(request.request_id, request.handoff, request.tokens, first_token, per_row, layers,
                      ready=ready)

    def _probe(self, layer_name: str) -> Any:
        """The layer's attention backend shape function, when vLLM exposes it."""
        context = getattr(self._vllm_config.compilation_config, "static_forward_context", {}) or {}
        backend = getattr(context.get(layer_name), "attn_backend", None)
        shape = getattr(backend, "get_kv_cache_shape", None)
        return None if shape is None else (lambda *args: tuple(shape(*args)))

    def _fill(self, export: Export, frame: int, out: memoryview) -> int:
        position, row_start, rows = export.frames[frame]
        layer = export.layers[position]
        with torch.cuda.stream(self._stream):
            self._stream.wait_event(export.ready)
            index = torch.tensor(layer.rows[row_start:row_start + rows], device=layer.tensor.device)
            pages = layer.tensor.index_select(layer.block_dim, index).contiguous()
            flat = pages.view(torch.uint8).reshape(-1)
            host = torch.frombuffer(out, dtype=torch.uint8, count=flat.numel())
            host.copy_(flat)
            self._stream.synchronize()
        return flat.numel()

    def _serve(self, link: str) -> None:
        rank, size = _tensor_parallel()
        # CUDA's current device is per thread; serve on the device that holds the caches.
        device = next(iter(self._caches.values())).device
        torch.cuda.set_device(device)
        while not self._stop.is_set():
            try:
                mailbox = ServiceMailbox(link)
            except MailboxError as exc:
                logger.info("MCDMA handoff: waiting for link %s: %s", link, exc)
                self._stop.wait(_REATTACH_S)
                continue
            logger.info("MCDMA handoff: serving rank %d of %d on link %s", rank, size, link)
            try:
                Responder(mailbox, self._table, self._fill, model=self._model, tp_rank=rank,
                          tp_size=size).run(self._stop)
            finally:
                mailbox.close()
            if not self._stop.is_set():
                time.sleep(1.0)
