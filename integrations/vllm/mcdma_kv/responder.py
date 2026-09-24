"""Answer one link's OPEN, PULL and CLOSE requests from the exports the connector registered."""

from __future__ import annotations

import json
import logging
import threading
import time
from collections.abc import Callable
from typing import Any

from . import wire

try:
    # python-isal computes the same zlib CRC-32 about three times faster on aarch64,
    # where zlib's checksum otherwise limits the handoff rate.
    from isal.isal_zlib import crc32 as _crc32
except ImportError:
    from zlib import crc32 as _crc32
from .export import Export, plan_frames

logger = logging.getLogger(__name__)
_NO_HANDOFF = bytes(16)


class ExportTable:
    """Exports by handoff, shared by the engine thread that adds them and the thread that serves them."""

    def __init__(self, ttl_s: float = 120.0, clock: Callable[[], float] = time.monotonic) -> None:
        self._ttl_s = ttl_s
        self._clock = clock
        self._lock = threading.Lock()
        self._exports: dict[bytes, Export | str] = {}
        self._owners: dict[bytes, tuple[str, float]] = {}
        self._finished: set[str] = set()

    def add(self, handoff: bytes, request_id: str, export: Export | str) -> None:
        """Hold an export, or the reason one could not be made, until it is closed or expires."""
        with self._lock:
            self._exports[handoff] = export
            self._owners[handoff] = (request_id, self._clock())

    def get(self, handoff: bytes) -> Export | str | None:
        with self._lock:
            return self._exports.get(handoff)

    def finish(self, handoff: bytes) -> None:
        """Release a handoff; its request's blocks are reported free at the next step."""
        with self._lock:
            self._exports.pop(handoff, None)
            owner = self._owners.pop(handoff, None)
            if owner is not None:
                self._finished.add(owner[0])

    def expire(self) -> None:
        """Release handoffs the decoder never closed."""
        now = self._clock()
        with self._lock:
            stale = [handoff for handoff, (_, added) in self._owners.items() if now - added > self._ttl_s]
        for handoff in stale:
            logger.warning("MCDMA handoff %s expired unclosed", handoff.hex())
            self.finish(handoff)

    def take_finished(self) -> set[str]:
        with self._lock:
            finished, self._finished = self._finished, set()
            return finished


class Responder:
    """Serves one link's mailbox; `fill` copies one frame's pages into the reply area and returns its size."""

    def __init__(self, mailbox: Any, table: ExportTable, fill: Callable[[Export, int, memoryview], int], *,
                 model: str, tp_rank: int, tp_size: int) -> None:
        self._mailbox = mailbox
        self._table = table
        self._fill = fill
        self._model = model
        self._tp = (tp_rank, tp_size)
        self._checksums: dict[bytes, bool] = {}

    def _reply(self, seq: int, kind: int, handoff: bytes, payload: bytes = b"", **fields: int) -> None:
        header = wire.Header(kind, handoff, nbytes=len(payload), **fields)
        self._mailbox.reply(seq, (wire.pack(header), payload))

    def handle(self, seq: int, payload: memoryview) -> None:
        """Answer one request."""
        try:
            header = wire.unpack(payload)
        except wire.WireError as exc:
            self._reply(seq, wire.ERROR, _NO_HANDOFF, str(exc).encode())
            return
        export = self._table.get(header.handoff)
        if isinstance(export, str):
            self._reply(seq, wire.ERROR, header.handoff, export.encode())
        elif header.kind == wire.OPEN:
            self._open(seq, header, payload, export)
        elif header.kind == wire.PULL:
            self._pull(seq, header, export)
        elif header.kind == wire.CLOSE:
            self._table.finish(header.handoff)
            self._checksums.pop(header.handoff, None)
            self._reply(seq, wire.ACK, header.handoff)
        else:
            self._reply(seq, wire.ERROR, header.handoff, f"unexpected request kind {header.kind}".encode())

    def _open(self, seq: int, header: wire.Header, payload: memoryview, export: Export | None) -> None:
        if export is None:
            # The decoder may ask before the step that finishes the prefill has registered it.
            self._reply(seq, wire.WAIT, header.handoff)
            return
        try:
            options = json.loads(bytes(wire.body(payload, header)) or b"{}")
        except ValueError:
            options = {}
        self._checksums[header.handoff] = bool(options.get("checksum", True))
        if not export.frames:
            capacity = self._mailbox.max_reply - wire.HEADER_BYTES
            export.frames = plan_frames(
                [(position, len(layer.rows), layer.row_bytes) for position, layer in enumerate(export.layers)],
                capacity)
        body = json.dumps(export.manifest(model=self._model, tp_rank=self._tp[0], tp_size=self._tp[1]))
        self._reply(seq, wire.MANIFEST, header.handoff, body.encode(), frames=len(export.frames))

    def _pull(self, seq: int, header: wire.Header, export: Export | None) -> None:
        if export is None or not 0 <= header.frame < len(export.frames):
            self._reply(seq, wire.ERROR, header.handoff, f"no frame {header.frame} in this handoff".encode())
            return
        position, row_start, rows = export.frames[header.frame]
        area = self._mailbox.reply_area()
        nbytes = self._fill(export, header.frame, area[wire.HEADER_BYTES:])
        checked = self._checksums.get(header.handoff, True)
        crc = _crc32(area[wire.HEADER_BYTES:wire.HEADER_BYTES + nbytes]) if checked else 0
        area[:wire.HEADER_BYTES] = wire.pack(wire.Header(
            wire.DATA, header.handoff, header.frame, len(export.frames), export.layers[position].index,
            wire.CHECKED if checked else 0, row_start, rows, nbytes, crc))
        self._mailbox.publish(seq, wire.HEADER_BYTES + nbytes)

    def run(self, stop: threading.Event, poll_s: float = 0.5) -> None:
        """Serve until `stop` is set or the daemon ends the registration."""
        while not stop.is_set():
            self._table.expire()
            if not self._mailbox.alive:
                return
            got = self._mailbox.next_request(poll_s)
            if got is None:
                continue
            seq, payload = got
            try:
                self.handle(seq, payload)
            except Exception as exc:
                logger.exception("MCDMA handoff request failed")
                self._reply(seq, wire.ERROR, _NO_HANDOFF, f"the producer failed: {exc}".encode())
