"""Loopback HTTP server for the convert service.

Endpoints (all require "Authorization: Bearer <token>"):

  GET  /health            service liveness and version
  POST /timeline/fcpxml   write a timeline payload as FCPXML 1.11
  POST /timeline/otio     write a timeline payload as OpenTimelineIO JSON
  POST /comfyui/import    map a ComfyUI workflow to canvas node specs
  POST /comfyui/export    map canvas node specs to a ComfyUI workflow
  POST /resolve/import    hand an interchange file to a running Resolve

The server binds to 127.0.0.1 only. The token comes from the parent process
and gates every request, so no other local process can drive the service.
"""

from __future__ import annotations

import hmac
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import __version__
from .comfy import WorkflowError, export_workflow, import_workflow
from .fcpxml import write_fcpxml
from .otio import write_otio
from .resolve_bridge import import_timeline
from .timeline_payload import PayloadError, parse_timeline_payload

MAX_BODY_BYTES = 8 << 20


class ConvertHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Set by build_server.
    token: str = ""

    def log_message(self, format: str, *args) -> None:
        print("convert-sidecar: " + format % args, file=sys.stderr, flush=True)

    def _authorized(self) -> bool:
        header = self.headers.get("Authorization", "")
        expected = f"Bearer {self.token}"
        return hmac.compare_digest(header.encode(), expected.encode())

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _reject_unauthorized(self) -> bool:
        if self._authorized():
            return False
        self._send_json(401, {"error": "unauthorized"})
        return True

    def do_GET(self) -> None:
        if self._reject_unauthorized():
            return
        if self.path == "/health":
            self._send_json(200, {"status": "serving", "version": __version__})
            return
        self._send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:
        # The body is drained on every path — auth rejections included —
        # because unread bytes would corrupt the next request on this
        # kept-alive connection.
        payload = self._read_body()
        if self._reject_unauthorized():
            return
        if payload is None:
            self._send_json(400, {"error": "invalid_body"})
            return
        if self.path == "/timeline/fcpxml":
            self._write_timeline(payload, write_fcpxml)
            return
        if self.path == "/timeline/otio":
            self._write_timeline(payload, write_otio)
            return
        if self.path == "/comfyui/import":
            self._comfy_import(payload)
            return
        if self.path == "/comfyui/export":
            self._comfy_export(payload)
            return
        if self.path == "/resolve/import":
            self._resolve_import(payload)
            return
        self._send_json(404, {"error": "not_found"})

    def _read_body(self) -> dict | None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if length <= 0:
            return {}
        remaining = length
        kept: list[bytes] = []
        kept_bytes = 0
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                return None
            remaining -= len(chunk)
            if kept_bytes <= MAX_BODY_BYTES:
                kept.append(chunk)
                kept_bytes += len(chunk)
        if length > MAX_BODY_BYTES:
            return None
        try:
            payload = json.loads(b"".join(kept))
        except json.JSONDecodeError:
            return None
        return payload if isinstance(payload, dict) else None

    def _write_timeline(self, payload: dict, writer) -> None:
        try:
            spec = parse_timeline_payload(payload)
        except PayloadError as error:
            self._send_json(400, {"error": "invalid_payload", "detail": str(error)})
            return
        try:
            path = writer(spec)
        except OSError as error:
            self._send_json(500, {"error": "write_failed", "detail": str(error)})
            return
        self._send_json(200, {"path": path})

    def _comfy_import(self, payload: dict) -> None:
        try:
            result = import_workflow(payload.get("workflow"))
        except WorkflowError as error:
            self._send_json(
                400, {"error": "invalid_workflow", "detail": str(error)}
            )
            return
        self._send_json(200, result)

    def _comfy_export(self, payload: dict) -> None:
        try:
            workflow = export_workflow(
                payload.get("nodes", []), payload.get("connections", [])
            )
        except WorkflowError as error:
            self._send_json(
                400, {"error": "invalid_nodes", "detail": str(error)}
            )
            return
        self._send_json(200, {"workflow": workflow})

    def _resolve_import(self, payload: dict) -> None:
        path = payload.get("path")
        if not isinstance(path, str) or not path:
            self._send_json(400, {"error": "invalid_payload", "detail": "path"})
            return
        self._send_json(200, import_timeline(path))


def build_server(token: str, port: int = 0) -> ThreadingHTTPServer:
    handler = type("BoundConvertHandler", (ConvertHandler,), {"token": token})
    return ThreadingHTTPServer(("127.0.0.1", port), handler)


def serve() -> int:
    token = os.environ.get("CUTPILOT_IPC_TOKEN", "")
    if not token:
        print("CUTPILOT_IPC_TOKEN is required", file=sys.stderr, flush=True)
        return 2
    port = int(os.environ.get("CUTPILOT_CONVERT_PORT", "0"))
    server = build_server(token, port)
    print(f"CUTPILOT_LISTENING {server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0
