"""Loopback HTTP server for the generation service.

Endpoints (all require "Authorization: Bearer <token>"):

  GET  /health              service liveness and version
  GET  /models              the model registry with per-provider key presence
  POST /jobs                submit a job; 409 when the model's key is missing
  GET  /jobs/<id>/events    server-sent events streaming job snapshots
  POST /jobs/<id>/cancel    request cooperative cancellation

The server binds to 127.0.0.1 only. The token comes from the parent process
and gates every request, so no other local process can drive the service.
"""

from __future__ import annotations

import hmac
import json
import os
import re
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import __version__, keys
from .jobs import TERMINAL_STATES, JobManager
from .registry import list_models, model_by_id

MIN_DIMENSION = 64
MAX_DIMENSION = 2048
DEFAULT_WIDTH = 768
DEFAULT_HEIGHT = 512
EVENT_WAIT_S = 0.25

_JOB_PATH = re.compile(r"^/jobs/([0-9a-f]+)/(events|cancel)$")


class GenerationHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Set by build_server.
    token: str = ""
    manager: JobManager = None  # type: ignore[assignment]

    def log_message(self, format: str, *args) -> None:
        print("generation-sidecar: " + format % args, file=sys.stderr, flush=True)

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
        if self.path == "/models":
            models = [
                {
                    "id": model.id,
                    "label": model.label,
                    "provider": model.provider,
                    "price_usd": model.price_usd,
                    "needs_key": model.needs_key,
                    "has_key": (not model.needs_key) or keys.has_key(model.provider),
                }
                for model in list_models()
            ]
            self._send_json(200, {"models": models})
            return
        match = _JOB_PATH.match(self.path)
        if match and match.group(2) == "events":
            self._stream_events(match.group(1))
            return
        self._send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:
        if self._reject_unauthorized():
            return
        if self.path == "/jobs":
            self._submit_job()
            return
        match = _JOB_PATH.match(self.path)
        if match and match.group(2) == "cancel":
            # The body is unused but must be drained: leaving it unread would
            # corrupt the next request on this kept-alive connection.
            self._read_body()
            if self.manager.cancel(match.group(1)):
                self._send_json(200, {"ok": True})
            else:
                self._send_json(404, {"error": "not_found"})
            return
        self._read_body()
        self._send_json(404, {"error": "not_found"})

    def _read_body(self) -> dict | None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length > 0 else b"{}"
            payload = json.loads(raw)
        except (ValueError, json.JSONDecodeError):
            return None
        return payload if isinstance(payload, dict) else None

    def _submit_job(self) -> None:
        payload = self._read_body()
        if payload is None:
            self._send_json(400, {"error": "invalid_body"})
            return

        model = model_by_id(str(payload.get("model", "")))
        if model is None:
            self._send_json(400, {"error": "unknown_model"})
            return

        prompt = str(payload.get("prompt", "")).strip()
        if not prompt:
            self._send_json(400, {"error": "empty_prompt"})
            return

        try:
            width = int(payload.get("width", DEFAULT_WIDTH))
            height = int(payload.get("height", DEFAULT_HEIGHT))
            seed = int(payload.get("seed", 0))
        except (TypeError, ValueError):
            self._send_json(400, {"error": "invalid_body"})
            return
        if not (
            MIN_DIMENSION <= width <= MAX_DIMENSION
            and MIN_DIMENSION <= height <= MAX_DIMENSION
        ):
            self._send_json(400, {"error": "invalid_size"})
            return

        if model.needs_key and not keys.has_key(model.provider):
            self._send_json(
                409, {"error": "missing_key", "provider": model.provider}
            )
            return

        job = self.manager.submit(model, prompt, width, height, seed)
        # Every accepted job enters the queue; the worker may already be past
        # that by the time this response is written, so the submission state
        # is reported as queued rather than sampled.
        self._send_json(
            202,
            {
                "job_id": job.id,
                "state": "queued",
                "estimated_cost_usd": model.price_usd,
            },
        )

    def _stream_events(self, job_id: str) -> None:
        job = self.manager.get(job_id)
        if job is None:
            self._send_json(404, {"error": "not_found"})
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        # The stream has no length; the connection closing marks its end.
        self.send_header("Connection", "close")
        self.close_connection = True
        self.end_headers()

        version = 0
        try:
            while True:
                snapshot = job.wait_newer(version, timeout=EVENT_WAIT_S)
                if snapshot.version == version:
                    continue  # timeout without change; poll the condition again
                version = snapshot.version
                frame = f"data: {json.dumps(snapshot.to_dict())}\n\n"
                self.wfile.write(frame.encode())
                self.wfile.flush()
                if snapshot.state in TERMINAL_STATES:
                    return
        except (BrokenPipeError, ConnectionResetError):
            return  # subscriber went away; the job keeps running


def build_server(token: str, gen_dir: str, port: int = 0) -> ThreadingHTTPServer:
    handler = type(
        "BoundGenerationHandler",
        (GenerationHandler,),
        {"token": token, "manager": JobManager(gen_dir)},
    )
    return ThreadingHTTPServer(("127.0.0.1", port), handler)


def serve() -> int:
    token = os.environ.get("CUTPILOT_IPC_TOKEN", "")
    if not token:
        print("CUTPILOT_IPC_TOKEN is required", file=sys.stderr, flush=True)
        return 2
    gen_dir = os.environ.get("CUTPILOT_GEN_DIR") or tempfile.mkdtemp(
        prefix="cutpilot-generations-"
    )
    port = int(os.environ.get("CUTPILOT_GEN_PORT", "0"))
    server = build_server(token, gen_dir, port)
    print(f"CUTPILOT_LISTENING {server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0
