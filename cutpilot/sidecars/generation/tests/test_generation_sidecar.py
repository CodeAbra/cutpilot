"""End-to-end tests for the generation sidecar over a live loopback server."""

import base64
import dataclasses
import hashlib
import ipaddress
import json
import http.client
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import tracemalloc
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from generation_sidecar import providers  # noqa: E402
from generation_sidecar.jobs import JobManager  # noqa: E402
from generation_sidecar.procedural import (  # noqa: E402
    MAX_INPUT_DIMENSION,
    decode_png,
    render_png,
)
from generation_sidecar.registry import (  # noqa: E402
    ModelInfo,
    list_models,
    model_by_id,
)
from generation_sidecar.server import build_server  # noqa: E402


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def _png_with_idat(width: int, height: int, raw: bytes, color: int = 2) -> bytes:
    """A structurally valid PNG whose IHDR declares width x height and whose
    IDAT carries the given decompressed payload — used to build inputs whose
    declared size and real payload deliberately disagree."""
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(raw, 9))
        + _png_chunk(b"IEND", b"")
    )

TOKEN = "test-token"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# A small valid PNG the stub vendor hands back, inline as base64 or via a URL.
STUB_PNG = _png_with_idat(4, 4, b"\x00" * ((4 * 3 + 1) * 4))
STUB_PNG_B64 = base64.b64encode(STUB_PNG).decode()

# A minimal blob whose first box is an ftyp header: enough to prove correct
# bytes landed at a .mp4 path without a real decoder (CI has none). The bytes at
# offset 4..8 are the ftyp tag a real container starts with.
MP4_FTYP = b"ftyp"
STUB_MP4 = b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom" + b"\x00" * 16


def _nest(path, value):
    """Build a nested dict/list structure that places value at path — string
    keys become dict levels and integers become list indices — so a stub can
    emit any vendor's response shape from a descriptor's path tuple."""
    if not path:
        return value
    head, rest = path[0], tuple(path[1:])
    if isinstance(head, int):
        items = [None] * (head + 1)
        items[head] = _nest(rest, value)
        return items
    return {head: _nest(rest, value)}


def _deep_merge(base, extra):
    """Merge extra into base, recursing into shared dict subtrees so a nested
    status path and a nested result path under one parent both survive."""
    for key, value in extra.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value
    return base


class StubVendor:
    """An in-process stand-in for a sync-image vendor. Records the last POST it
    received (headers + parsed JSON body) and returns a canned (status, json).
    In url mode it also serves the returned PNG at /img/<name>.png. Bound to
    127.0.0.1 on an ephemeral port; never reaches the network."""

    def __init__(self, status=200, body=None, redirect_location=None):
        self.status = status
        self.body = body if body is not None else {"data": [{"b64_json": STUB_PNG_B64}]}
        self.redirect_location = redirect_location
        self.last_headers = None
        self.last_body = None
        vendor = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length) if length > 0 else b""
                vendor.last_headers = dict(self.headers)
                try:
                    vendor.last_body = json.loads(raw) if raw else {}
                except json.JSONDecodeError:
                    vendor.last_body = None
                payload = json.dumps(vendor.body).encode()
                self.send_response(vendor.status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                if self.path.startswith("/img/"):
                    if vendor.redirect_location is not None:
                        self.send_response(302)
                        self.send_header("Location", vendor.redirect_location)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Content-Length", str(len(STUB_PNG)))
                    self.end_headers()
                    self.wfile.write(STUB_PNG)
                    return
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        self.url = f"http://127.0.0.1:{self.port}"
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()


class StubAsyncVendor:
    """An in-process stand-in for a submit -> poll -> fetch vendor. The submit
    returns a job id and a loopback polling URL pointing back at this server, so
    the poll is served locally and never touches the network. Modes drive the
    poll replies: 'ready' returns Pending for a few polls then Ready with a
    result URL; 'failure' returns an error state; 'never' stays Pending; 'cancel'
    returns Pending with a cancel URL and records whether its cancel route was
    hit; 'ssrf' returns a polling URL aimed at the cloud-metadata host. Bound to
    127.0.0.1 on an ephemeral port."""

    def __init__(
        self,
        mode="ready",
        polls_before_ready=2,
        polling_url=None,
        status_path=("status",),
        running_value="Pending",
        success_value="Ready",
        failure_value="Error",
        result_kind="image",
        result_ref_path=None,
        video_body=None,
        submit_id_path=("id",),
        error_status=None,
    ):
        self.mode = mode
        self.polls_before_ready = polls_before_ready
        self.error_status = error_status
        self.injected_polling_url = polling_url
        self.submit_id_path = tuple(submit_id_path)
        self.status_path = tuple(status_path)
        self.running_value = running_value
        self.success_value = success_value
        self.failure_value = failure_value
        self.result_kind = result_kind
        if result_ref_path is None:
            result_ref_path = (
                ("result", "sample") if result_kind == "image" else ("output", 0)
            )
        self.result_ref_path = tuple(result_ref_path)
        self.video_body = video_body if video_body is not None else STUB_MP4
        self.submit_headers = None
        self.submit_body = None
        self.submit_raw = None
        self.submit_path = None
        self.poll_headers = None
        self.poll_count = 0
        self.cancel_hit = False
        vendor = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def _reply(self, payload, content_type="application/json"):
                if content_type == "application/json":
                    body = json.dumps(payload).encode()
                else:
                    body = payload
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _reply_status(self, status, body, content_type):
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if body:
                    self.wfile.write(body)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length) if length > 0 else b""
                if self.path.startswith("/cancel/"):
                    vendor.cancel_hit = True
                    self._reply({"ok": True})
                    return
                vendor.submit_headers = dict(self.headers)
                vendor.submit_path = self.path
                vendor.submit_raw = raw
                try:
                    vendor.submit_body = json.loads(raw) if raw else {}
                except (json.JSONDecodeError, UnicodeDecodeError):
                    # A multipart submit body is binary, not JSON; submit_raw
                    # keeps the exact bytes for assertions.
                    vendor.submit_body = None
                if vendor.mode == "ssrf":
                    polling_url = "https://169.254.169.254/poll/1"
                elif vendor.injected_polling_url is not None:
                    polling_url = vendor.injected_polling_url
                else:
                    polling_url = f"{vendor.url}/poll/1"
                response = _nest(vendor.submit_id_path, "1")
                _deep_merge(response, {"polling_url": polling_url})
                self._reply(response)

            def do_GET(self):
                if self.path.startswith("/img/"):
                    self._reply(STUB_PNG, content_type="image/png")
                    return
                if self.path.startswith("/video/"):
                    self._reply(vendor.video_body, content_type="video/mp4")
                    return
                if vendor.mode == "stability":
                    # Completion signaled by HTTP status: 202 while running, then
                    # a 200 whose body is the mp4 (no separate result fetch). A
                    # configured error_status makes the poll return a terminal
                    # client/server error instead.
                    vendor.poll_headers = dict(self.headers)
                    vendor.poll_count += 1
                    if vendor.error_status is not None:
                        self._reply_status(
                            vendor.error_status, b"", "application/octet-stream"
                        )
                    elif vendor.poll_count <= vendor.polls_before_ready:
                        self._reply_status(202, b"", "application/octet-stream")
                    else:
                        self._reply_status(200, vendor.video_body, "video/mp4")
                    return
                # Any other GET is a poll for this job, whatever path the row's
                # poll_path resolves to.
                vendor.poll_headers = dict(self.headers)
                vendor.poll_count += 1
                self._reply(vendor._poll_payload())

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        self.url = f"http://127.0.0.1:{self.port}"
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def _running_payload(self):
        return _nest(self.status_path, self.running_value)

    def _success_payload(self):
        if self.result_kind == "video":
            url = f"{self.url}/video/1.mp4"
        else:
            url = f"{self.url}/img/1.png"
        payload = _nest(self.status_path, self.success_value)
        return _deep_merge(payload, _nest(self.result_ref_path, url))

    def _poll_payload(self):
        if self.mode == "failure":
            payload = _nest(self.status_path, self.failure_value)
            payload["details"] = "quota exceeded"
            return payload
        if self.mode == "never":
            return self._running_payload()
        if self.mode == "cancel":
            payload = self._running_payload()
            payload["cancel_url"] = f"{self.url}/cancel/1"
            return payload
        if self.mode == "zero_progress":
            # Report a freshly-queued job at progress 0.0 for a few polls, then
            # a ready result URL: exercises the submit->poll progress seam.
            if self.poll_count <= self.polls_before_ready:
                payload = self._running_payload()
                payload["progress"] = 0.0
                return payload
            return self._success_payload()
        # ready / ssrf: report progress while pending, then a ready result URL.
        if self.poll_count <= self.polls_before_ready:
            payload = self._running_payload()
            payload["progress"] = self.poll_count / (self.polls_before_ready + 1)
            return payload
        return self._success_payload()

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()


class SidecarTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Key lookups must be deterministic: env-only, with no vendor key set.
        os.environ["CUTPILOT_DISABLE_KEYCHAIN"] = "1"
        os.environ.pop("OPENAI_API_KEY", None)

        cls.gen_dir = tempfile.mkdtemp(prefix="cutpilot-gen-test-")
        cls.server = build_server(TOKEN, cls.gen_dir)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        # The in-process stubs bind to 127.0.0.1 over http; enable the
        # loopback-http transport carve-out for the duration of each test. In
        # production the flag stays off so a vendor-supplied loopback URL is
        # refused.
        loopback = patch.object(providers, "_ALLOW_LOOPBACK_HTTP", True)
        loopback.start()
        self.addCleanup(loopback.stop)

    def request(self, method, path, body=None, token=TOKEN):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        headers = {}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        payload = None
        if body is not None:
            payload = json.dumps(body)
            headers["Content-Type"] = "application/json"
        connection.request(method, path, body=payload, headers=headers)
        response = connection.getresponse()
        data = json.loads(response.read())
        connection.close()
        return response.status, data

    def stream_events(self, job_id, on_event):
        """Feed each SSE snapshot to on_event until it returns False or the
        stream reaches a terminal state; returns all snapshots seen."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        connection.request(
            "GET",
            f"/jobs/{job_id}/events",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        snapshots = []
        while True:
            line = response.readline()
            if not line:
                break
            line = line.strip()
            if not line.startswith(b"data: "):
                continue
            snapshot = json.loads(line[len(b"data: "):])
            snapshots.append(snapshot)
            keep_going = on_event(snapshot)
            if keep_going is False or snapshot["state"] in ("done", "error", "canceled"):
                break
        connection.close()
        return snapshots

    def submit(self, **overrides):
        body = {
            "model": "local/procedural-v1",
            "prompt": "a lighthouse at dusk",
            "width": 192,
            "height": 128,
            "seed": 7,
        }
        body.update(overrides)
        return self.request("POST", "/jobs", body=body)

    def test_rejects_missing_and_wrong_tokens(self):
        for token in (None, "wrong-token"):
            status, data = self.request("GET", "/health", token=token)
            self.assertEqual(status, 401)
            self.assertEqual(data["error"], "unauthorized")

    def test_unauthorized_post_body_does_not_poison_the_connection(self):
        # The rejected request's body must be drained: with keep-alive, unread
        # bytes would be parsed as the start of the next request on this
        # same connection.
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        payload = json.dumps({"model": "local/procedural-v1", "prompt": "x" * 512})
        connection.request(
            "POST",
            "/jobs",
            body=payload,
            headers={
                "Authorization": "Bearer wrong-token",
                "Content-Type": "application/json",
            },
        )
        response = connection.getresponse()
        self.assertEqual(response.status, 401)
        response.read()

        connection.request(
            "GET", "/health", headers={"Authorization": f"Bearer {TOKEN}"}
        )
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(json.loads(response.read())["status"], "serving")
        connection.close()

    def test_health_reports_serving(self):
        status, data = self.request("GET", "/health")
        self.assertEqual(status, 200)
        self.assertEqual(data["status"], "serving")
        self.assertTrue(data["version"])

    def test_models_lists_registry_with_key_presence(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        by_id = {model["id"]: model for model in data["models"]}
        self.assertIn("local/procedural-v1", by_id)
        self.assertIn("openai/gpt-image-1", by_id)
        self.assertTrue(by_id["local/procedural-v1"]["has_key"])
        self.assertFalse(by_id["openai/gpt-image-1"]["has_key"])
        self.assertGreater(by_id["local/procedural-v1"]["price_usd"], 0.0)

    def test_local_job_runs_to_completion(self):
        status, submitted = self.submit()
        self.assertEqual(status, 202)
        self.assertEqual(submitted["state"], "queued")
        self.assertGreater(submitted["estimated_cost_usd"], 0.0)

        progress_seen = []
        snapshots = self.stream_events(
            submitted["job_id"],
            lambda snap: progress_seen.append(snap["progress"]) or True,
        )

        states = [snap["state"] for snap in snapshots]
        self.assertEqual(states[-1], "done")
        self.assertIn("running", states)
        # Progress never regresses across the stream.
        self.assertEqual(progress_seen, sorted(progress_seen))

        final = snapshots[-1]
        self.assertAlmostEqual(final["cost_usd"], 0.002)
        self.assertEqual((final["width"], final["height"]), (192, 128))
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertTrue(content.startswith(PNG_SIGNATURE))

    def test_same_inputs_produce_identical_images(self):
        digests = []
        for _ in range(2):
            _, submitted = self.submit(seed=42)
            snapshots = self.stream_events(submitted["job_id"], lambda snap: True)
            with open(snapshots[-1]["result_path"], "rb") as handle:
                digests.append(hashlib.sha256(handle.read()).hexdigest())
        self.assertEqual(digests[0], digests[1])

    def test_cancel_stops_an_in_flight_job(self):
        _, submitted = self.submit(width=512, height=384)
        job_id = submitted["job_id"]

        def cancel_on_first_running(snapshot):
            if snapshot["state"] == "running":
                status, data = self.request("POST", f"/jobs/{job_id}/cancel")
                self.assertEqual(status, 200)
                self.assertTrue(data["ok"])
            return True

        snapshots = self.stream_events(job_id, cancel_on_first_running)
        self.assertEqual(snapshots[-1]["state"], "canceled")
        self.assertEqual(snapshots[-1]["message"], "Stopped")
        self.assertNotIn("done", [snap["state"] for snap in snapshots])

    def test_missing_key_refuses_the_job(self):
        status, data = self.submit(model="openai/gpt-image-1")
        self.assertEqual(status, 409)
        self.assertEqual(data["error"], "missing_key")
        self.assertEqual(data["provider"], "openai")

    def test_invalid_submissions_are_rejected(self):
        status, data = self.submit(model="nonexistent/model")
        self.assertEqual(status, 400)
        self.assertEqual(data["error"], "unknown_model")

        status, data = self.submit(prompt="   ")
        self.assertEqual(status, 400)
        self.assertEqual(data["error"], "empty_prompt")

        status, data = self.submit(width=16)
        self.assertEqual(status, 400)
        self.assertEqual(data["error"], "invalid_size")

        status, data = self.request("POST", "/jobs/unknown/cancel")
        self.assertEqual(status, 404)

    def test_events_for_unknown_job_are_not_found(self):
        status, data = self.request("GET", "/jobs/feedbeef/events")
        self.assertEqual(status, 404)
        self.assertEqual(data["error"], "not_found")

    def run_to_done(self, **overrides):
        status, submitted = self.submit(**overrides)
        self.assertEqual(status, 202)
        snapshots = self.stream_events(submitted["job_id"], lambda snap: True)
        return snapshots[-1]

    def render_input(self, name="input.png", width=96, height=64, seed=3):
        path = os.path.join(self.gen_dir, name)
        render_png("an input plate", seed, width, height, path)
        return path

    def set_env_key(self, name, value="test"):
        os.environ[name] = value
        self.addCleanup(lambda: os.environ.pop(name, None))

    def redirect_descriptor(self, provider, stub, **overrides):
        """Point a provider's descriptor at an in-process stub for the duration
        of the test, leaving every other field intact."""
        base_row = providers.SYNC_IMAGE_DESCRIPTORS[provider]
        replaced = dataclasses.replace(base_row, base_url=stub.url, **overrides)
        patcher = patch.dict(
            providers.SYNC_IMAGE_DESCRIPTORS, {provider: replaced}
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def redirect_async_descriptor(self, stub, **overrides):
        """Point the BFL async descriptor at an in-process stub with fast
        timings for the duration of the test, leaving every other field intact."""
        replaced = dataclasses.replace(
            providers.ASYNC_JOB_DESCRIPTORS["bfl"],
            base_url=stub.url,
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            **overrides,
        )
        patcher = patch.dict(providers.ASYNC_JOB_DESCRIPTORS, {"bfl": replaced})
        patcher.start()
        self.addCleanup(patcher.stop)

    def redirect_async_row(self, descriptor_id, stub, **overrides):
        """Point an arbitrary async descriptor at an in-process stub with fast
        timings for the duration of the test, leaving every other field intact."""
        replaced = dataclasses.replace(
            providers.ASYNC_JOB_DESCRIPTORS[descriptor_id],
            base_url=stub.url,
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            **overrides,
        )
        patcher = patch.dict(
            providers.ASYNC_JOB_DESCRIPTORS, {descriptor_id: replaced}
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def _video_stub_for(self, descriptor_id, **kw):
        """A video-result stub whose status and result shapes mirror a row's
        descriptor so the real submit->poll->fetch loop runs against it."""
        desc = providers.ASYNC_JOB_DESCRIPTORS[descriptor_id]
        return StubAsyncVendor(
            result_kind="video",
            status_path=desc.status_path,
            success_value=desc.success_states[0],
            failure_value=(desc.failure_states or ("failed",))[0],
            running_value="in_progress",
            result_ref_path=desc.result_ref_path,
            submit_id_path=desc.job_id_path,
            **kw,
        )

    def _drive_video_row(self, model_id, descriptor_id, env_var, slug, needs_input):
        self.set_env_key(env_var)
        model = model_by_id(model_id)
        if descriptor_id == "stability":
            stub = StubAsyncVendor(mode="stability", polls_before_ready=2).start()
        else:
            stub = self._video_stub_for(
                descriptor_id, mode="ready", polls_before_ready=2
            ).start()
        self.addCleanup(stub.stop)
        self.redirect_async_row(descriptor_id, stub, job_deadline_s=5.0)

        overrides = {
            "model": model_id,
            "prompt": "a lighthouse at dusk",
            "width": 1024,
            "height": 1024,
        }
        if needs_input:
            overrides["input_path"] = self.render_input(name=f"{descriptor_id}-in.png")
        progress = []
        status, submitted = self.submit(**overrides)
        self.assertEqual(status, 202)
        snapshots = self.stream_events(
            submitted["job_id"],
            lambda snap: progress.append(snap["progress"]) or True,
        )
        final = snapshots[-1]
        self.assertEqual(final["state"], "done")
        self.assertTrue(final["result_path"].endswith(".mp4"))
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertEqual(content[4:8], MP4_FTYP)
        self.assertEqual(final["result_digest"], hashlib.sha256(content).hexdigest())
        self.assertAlmostEqual(final["cost_usd"], model.price_usd)
        self.assertEqual(progress, sorted(progress))
        pre_terminal = [
            snap["progress"] for snap in snapshots if snap["state"] != "done"
        ]
        self.assertTrue(all(value < 1.0 for value in pre_terminal))
        # The key rides submit and poll; the result fetch (url rows) carries no
        # auth header — the stub's asset routes require none.
        self.assertEqual(stub.submit_headers.get("Authorization"), "Bearer test")
        self.assertEqual(stub.poll_headers.get("Authorization"), "Bearer test")
        # The slug is read from the row, never hardcoded: it rides the body for
        # the json rows; Stability carries no slug and posts multipart.
        if descriptor_id == "stability":
            self.assertIn(
                "multipart/form-data", stub.submit_headers.get("Content-Type", "")
            )
            self.assertEqual(stub.submit_headers.get("accept"), "video/*")
        else:
            self.assertIn(slug, json.dumps(stub.submit_body))
        return stub, final

    def test_openai_request_stays_byte_identical(self):
        # The refactor must not change the bytes OpenAI receives: same four
        # body keys in the same order, no response_format, fixed-set sizing.
        self.set_env_key("OPENAI_API_KEY")
        stub = StubVendor().start()
        self.addCleanup(stub.stop)
        self.redirect_descriptor("openai", stub)

        final = self.run_to_done(
            model="openai/gpt-image-1",
            prompt="a lighthouse at dusk",
            width=1024,
            height=1024,
        )
        self.assertEqual(final["state"], "done")
        self.assertEqual(stub.last_headers.get("Authorization"), "Bearer test")
        self.assertEqual(stub.last_headers.get("Content-Type"), "application/json")
        self.assertEqual(
            stub.last_body,
            {
                "model": "gpt-image-1",
                "prompt": "a lighthouse at dusk",
                "size": "1024x1024",
                "n": 1,
            },
        )
        self.assertNotIn("response_format", stub.last_body)
        self.assertEqual(list(stub.last_body.keys()), ["model", "prompt", "size", "n"])

        landscape = self.run_to_done(
            model="openai/gpt-image-1",
            prompt="a wide vista",
            width=1536,
            height=1024,
        )
        self.assertEqual(landscape["state"], "done")
        self.assertEqual(stub.last_body["size"], "1536x1024")

    def _drive_b64_provider(self, model_id, provider, env_var, slug):
        self.set_env_key(env_var)
        stub = StubVendor(body={"data": [{"b64_json": STUB_PNG_B64}]}).start()
        self.addCleanup(stub.stop)
        self.redirect_descriptor(provider, stub)

        width, height = 1024, 768
        progress = []
        status, submitted = self.submit(
            model=model_id, prompt="a neon skyline", width=width, height=height
        )
        self.assertEqual(status, 202)
        snapshots = self.stream_events(
            submitted["job_id"],
            lambda snap: progress.append(snap["progress"]) or True,
        )
        final = snapshots[-1]

        self.assertEqual(final["state"], "done")
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertTrue(content.startswith(PNG_SIGNATURE))
        self.assertEqual(final["result_digest"], hashlib.sha256(content).hexdigest())
        self.assertEqual((final["width"], final["height"]), (width, height))
        self.assertAlmostEqual(final["cost_usd"], model_by_id(model_id).price_usd)
        self.assertEqual(progress, sorted(progress))

        self.assertEqual(stub.last_headers.get("Authorization"), "Bearer test")
        self.assertEqual(stub.last_body["model"], slug)
        self.assertEqual(stub.last_body["prompt"], "a neon skyline")
        self.assertEqual(stub.last_body["size"], f"{width}x{height}")
        self.assertEqual(stub.last_body["response_format"], "b64_json")

    def test_recraft_generates_through_its_stub(self):
        self._drive_b64_provider(
            "recraft/recraftv4_1", "recraft", "RECRAFT_API_TOKEN", "recraftv4_1"
        )

    def test_nano_banana_generates_through_its_stub(self):
        self._drive_b64_provider(
            "google/gemini-2.5-flash-image",
            "google",
            "GEMINI_API_KEY",
            "gemini-2.5-flash-image",
        )

    def test_new_sync_providers_appear_in_models_with_key_presence(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        by_id = {model["id"]: model for model in data["models"]}
        for model_id in ("recraft/recraftv4_1", "google/gemini-2.5-flash-image"):
            self.assertIn(model_id, by_id)
            self.assertFalse(by_id[model_id]["has_key"])

        self.set_env_key("RECRAFT_API_TOKEN")
        status, data = self.request("GET", "/models")
        by_id = {model["id"]: model for model in data["models"]}
        self.assertTrue(by_id["recraft/recraftv4_1"]["has_key"])
        self.assertFalse(by_id["google/gemini-2.5-flash-image"]["has_key"])

    def test_new_sync_providers_refuse_without_key(self):
        for model_id, provider in (
            ("recraft/recraftv4_1", "recraft"),
            ("google/gemini-2.5-flash-image", "google"),
        ):
            status, data = self.submit(model=model_id)
            self.assertEqual(status, 409)
            self.assertEqual(data["error"], "missing_key")
            self.assertEqual(data["provider"], provider)

    def test_vendor_failure_surfaces_message_without_leaking_key(self):
        self.set_env_key("RECRAFT_API_TOKEN")
        stub = StubVendor(
            status=400, body={"error": {"message": "quota exceeded"}}
        ).start()
        self.addCleanup(stub.stop)
        self.redirect_descriptor("recraft", stub)

        final = self.run_to_done(
            model="recraft/recraftv4_1", prompt="x", width=1024, height=768
        )
        self.assertEqual(final["state"], "error")
        self.assertIn("quota exceeded", final["message"])
        self.assertNotIn("test", final["message"])

    def test_seedream_url_mode_fetches_and_writes_the_image(self):
        self.set_env_key("ARK_API_KEY")
        stub = StubVendor()
        stub.body = {"data": [{"url": stub.url + "/img/x.png"}]}
        stub.start()
        self.addCleanup(stub.stop)
        self.redirect_descriptor("bytedance", stub)

        final = self.run_to_done(
            model="bytedance/seedream-4-0",
            prompt="a dune sea",
            width=1024,
            height=1024,
        )
        self.assertEqual(final["state"], "done")
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertTrue(content.startswith(PNG_SIGNATURE))
        self.assertEqual(final["result_digest"], hashlib.sha256(content).hexdigest())
        self.assertEqual(stub.last_headers.get("Authorization"), "Bearer test")
        self.assertEqual(stub.last_body["model"], "seedream-4-0")

    def test_async_engine_submit_poll_ready_writes_image_and_carries_x_key(self):
        stub = StubAsyncVendor(mode="ready", polls_before_ready=2).start()
        self.addCleanup(stub.stop)
        desc = providers.AsyncJobDescriptor(
            provider="stub",
            base_url=stub.url,
            auth_header="x-key",
            auth_template="{key}",
            success_states=("Ready",),
            failure_states=("Error", "Failed"),
            progress_path=("progress",),
            error_msg_path=("details",),
            result_ref_path=("result", "sample"),
            result_fetch="url",
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            job_deadline_s=2.0,
        )
        model = ModelInfo(
            id="stub/async-1",
            label="Stub Async",
            provider="stub",
            price_usd=0.05,
            needs_key=True,
        )
        out_path = os.path.join(self.gen_dir, "async-engine.png")
        request = providers.GenerationRequest(
            model=model,
            prompt="a lighthouse at dusk",
            width=1024,
            height=1024,
            seed=7,
            out_path=out_path,
        )
        progress = []
        with patch.dict(providers.ASYNC_JOB_DESCRIPTORS, {"stub": desc}), patch.object(
            providers.keys, "lookup_key", return_value="secret-key"
        ):
            result = providers.AsyncJobProvider().generate(
                request,
                on_progress=progress.append,
                is_canceled=lambda: False,
            )

        self.assertEqual(result.path, out_path)
        with open(out_path, "rb") as handle:
            content = handle.read()
        self.assertTrue(content.startswith(PNG_SIGNATURE))
        self.assertEqual(stub.submit_body["prompt"], "a lighthouse at dusk")
        self.assertEqual(stub.submit_body["width"], 1024)
        self.assertEqual(stub.submit_body["height"], 1024)
        self.assertEqual(stub.submit_headers.get("x-key"), "secret-key")
        self.assertNotIn("Authorization", stub.submit_headers)
        self.assertEqual(stub.poll_headers.get("x-key"), "secret-key")
        self.assertNotIn("Authorization", stub.poll_headers)
        self.assertEqual(progress, sorted(progress))
        self.assertTrue(all(value < 1.0 for value in progress))

    def test_async_progress_never_regresses_below_the_submit_floor(self):
        # The engine streams 0.02 before submit; a vendor that reports
        # progress 0.0 on the first poll must not drag the stream back below
        # that floor.
        stub = StubAsyncVendor(mode="zero_progress", polls_before_ready=2).start()
        self.addCleanup(stub.stop)
        desc = providers.AsyncJobDescriptor(
            provider="stub",
            base_url=stub.url,
            auth_header="x-key",
            auth_template="{key}",
            success_states=("Ready",),
            failure_states=("Error", "Failed"),
            progress_path=("progress",),
            result_ref_path=("result", "sample"),
            result_fetch="url",
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            job_deadline_s=2.0,
        )
        model = ModelInfo(
            id="stub/async-1",
            label="Stub Async",
            provider="stub",
            price_usd=0.05,
            needs_key=True,
        )
        out_path = os.path.join(self.gen_dir, "async-floor.png")
        request = providers.GenerationRequest(
            model=model,
            prompt="a lighthouse at dusk",
            width=1024,
            height=1024,
            seed=7,
            out_path=out_path,
        )
        progress = []
        with patch.dict(providers.ASYNC_JOB_DESCRIPTORS, {"stub": desc}), patch.object(
            providers.keys, "lookup_key", return_value="secret-key"
        ):
            providers.AsyncJobProvider().generate(
                request,
                on_progress=progress.append,
                is_canceled=lambda: False,
            )
        self.assertEqual(progress[0], 0.02)
        self.assertEqual(progress, sorted(progress))
        self.assertTrue(all(value >= 0.02 for value in progress))
        self.assertTrue(all(value < 1.0 for value in progress))

    def test_async_job_submit_poll_ready_writes_image(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="ready", polls_before_ready=2).start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(stub)

        progress = []
        status, submitted = self.submit(
            model="bfl/flux-2-pro-preview",
            prompt="a lighthouse at dusk",
            width=1024,
            height=1024,
        )
        self.assertEqual(status, 202)
        snapshots = self.stream_events(
            submitted["job_id"],
            lambda snap: progress.append(snap["progress"]) or True,
        )
        final = snapshots[-1]
        self.assertEqual(final["state"], "done")
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertTrue(content.startswith(PNG_SIGNATURE))
        self.assertEqual(final["result_digest"], hashlib.sha256(content).hexdigest())
        self.assertEqual((final["width"], final["height"]), (1024, 1024))
        self.assertAlmostEqual(final["cost_usd"], 0.05)
        self.assertEqual(progress, sorted(progress))

        # The slug rides the submit path; the raw key rides an x-key header on
        # both submit and poll, and no Authorization header is ever sent.
        self.assertEqual(stub.submit_path, "/flux-2-pro-preview")
        self.assertEqual(stub.submit_body["prompt"], "a lighthouse at dusk")
        self.assertEqual(stub.submit_body["width"], 1024)
        self.assertEqual(stub.submit_body["height"], 1024)
        self.assertEqual(stub.submit_headers.get("x-key"), "test")
        self.assertNotIn("Authorization", stub.submit_headers)
        self.assertEqual(stub.poll_headers.get("x-key"), "test")
        self.assertNotIn("Authorization", stub.poll_headers)

        # A size off the 32-grid is rounded to the nearest multiple before submit
        # and the rounded size is what the result reports.
        rounding_stub = StubAsyncVendor(mode="ready", polls_before_ready=1).start()
        self.addCleanup(rounding_stub.stop)
        self.redirect_async_descriptor(rounding_stub)
        rounded_final = self.run_to_done(
            model="bfl/flux-2-pro-preview",
            prompt="a wide vista",
            width=1000,
            height=1000,
        )
        self.assertEqual(rounded_final["state"], "done")
        self.assertEqual(rounding_stub.submit_body["width"], 992)
        self.assertEqual(rounding_stub.submit_body["height"], 992)
        self.assertEqual((rounded_final["width"], rounded_final["height"]), (992, 992))

    def test_bfl_is_excluded_from_models_yet_resolves(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        ids = {model["id"] for model in data["models"]}
        self.assertNotIn("bfl/flux-2-pro-preview", ids)

        # A direct submission still resolves the row: an absent key returns the
        # missing-key refusal, not the unknown-model rejection.
        status, data = self.submit(model="bfl/flux-2-pro-preview")
        self.assertEqual(status, 409)
        self.assertEqual(data["error"], "missing_key")
        self.assertEqual(data["provider"], "bfl")

    def test_unverified_model_is_excluded_from_models_yet_resolves(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        ids = {model["id"] for model in data["models"]}
        self.assertNotIn("bytedance/seedream-4-0", ids)

        # A direct submission still resolves the row: an absent key returns the
        # missing-key refusal, not the unknown-model rejection.
        status, data = self.submit(model="bytedance/seedream-4-0")
        self.assertEqual(status, 409)
        self.assertEqual(data["error"], "missing_key")
        self.assertEqual(data["provider"], "bytedance")

    def test_descriptor_routes_two_shapes_under_one_provider(self):
        # One provider id (bytedance) hosts two shapes: the synchronous Seedream
        # image row and an asynchronous video row. The descriptor, not the
        # provider, selects the adapter, so the two resolve to different
        # singletons without either regressing.
        synthetic = providers.AsyncJobDescriptor(
            provider="bytedance",
            base_url="https://ark.example/api/v3",
            success_states=("succeeded",),
            failure_states=("failed",),
            result_ref_path=("content", "video_url"),
            result_fetch="url",
        )
        video_model = ModelInfo(
            id="bytedance/seedance-stub",
            label="Seedance Stub",
            provider="bytedance",
            price_usd=0.1,
            needs_key=True,
            descriptor="seedance-video",
            output_kind="video",
        )
        with patch.dict(
            providers.ASYNC_JOB_DESCRIPTORS, {"seedance-video": synthetic}
        ):
            self.assertIs(
                providers.provider_for(video_model), providers._async_job
            )
            self.assertIs(
                providers.provider_for(model_by_id("bytedance/seedream-4-0")),
                providers._sync_image,
            )
            # A provider-keyed async row (BFL, no descriptor) still resolves via
            # the provider-id fallback.
            self.assertIs(
                providers.provider_for(model_by_id("bfl/flux-2-pro-preview")),
                providers._async_job,
            )

    def _wait_job(self, job, timeout=10.0):
        deadline = time.monotonic() + timeout
        version = 0
        while time.monotonic() < deadline:
            snapshot = job.wait_newer(version, timeout=0.5)
            version = snapshot.version
            if snapshot.state in ("done", "error", "canceled"):
                return snapshot
        raise AssertionError("job did not settle in time")

    def _run_async_direct(self, desc, model, out_name, **request_overrides):
        out_path = os.path.join(self.gen_dir, out_name)
        fields = {
            "prompt": "a lighthouse at dusk",
            "width": 1024,
            "height": 1024,
            "seed": 7,
        }
        fields.update(request_overrides)
        request = providers.GenerationRequest(
            model=model, out_path=out_path, **fields
        )
        progress = []
        with patch.dict(
            providers.ASYNC_JOB_DESCRIPTORS, {"stub": desc}
        ), patch.object(providers.keys, "lookup_key", return_value="secret-key"):
            result = providers.AsyncJobProvider().generate(
                request, on_progress=progress.append, is_canceled=lambda: False
            )
        return result, progress

    def _stub_async_descriptor(self, stub, **overrides):
        fields = dict(
            provider="stub",
            base_url=stub.url,
            auth_header="x-key",
            auth_template="{key}",
            success_states=("Ready",),
            failure_states=("Error", "Failed"),
            result_ref_path=("output", 0),
            result_fetch="url",
            result_max_bytes=256 * 1024 * 1024,
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            job_deadline_s=2.0,
        )
        fields.update(overrides)
        return providers.AsyncJobDescriptor(**fields)

    def test_async_slug_rides_the_body_when_configured(self):
        stub = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(stub, model_body_key="model")
        model = ModelInfo(
            id="stub/video-1",
            label="Stub Video",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            model_slug="stub-video-slug",
            output_kind="video",
        )
        result, _ = self._run_async_direct(desc, model, "slug-in-body.mp4")
        self.assertEqual(stub.submit_body["model"], providers._slug(model))
        with open(result.path, "rb") as handle:
            self.assertEqual(handle.read()[4:8], MP4_FTYP)

        # A descriptor without model_body_key omits the model key (BFL parity).
        stub2 = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(stub2.stop)
        desc2 = self._stub_async_descriptor(stub2, model_body_key=None)
        self._run_async_direct(desc2, model, "no-slug.mp4")
        self.assertNotIn("model", stub2.submit_body)

    def test_result_extension_follows_output_kind(self):
        manager = JobManager(self.gen_dir)

        video_stub = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(video_stub.stop)
        video_desc = self._stub_async_descriptor(video_stub)
        video_model = ModelInfo(
            id="stub/video-1",
            label="Stub Video",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            output_kind="video",
        )
        with patch.dict(
            providers.ASYNC_JOB_DESCRIPTORS, {"stub": video_desc}
        ), patch.object(providers.keys, "lookup_key", return_value="secret-key"):
            job = manager.submit(video_model, "a lighthouse", 1024, 1024, 7)
            final = self._wait_job(job)
        self.assertEqual(final.state, "done")
        self.assertTrue(final.result_path.endswith(".mp4"))
        with open(final.result_path, "rb") as handle:
            self.assertEqual(handle.read()[4:8], MP4_FTYP)

        image_stub = StubAsyncVendor(mode="ready", polls_before_ready=1).start()
        self.addCleanup(image_stub.stop)
        image_desc = self._stub_async_descriptor(
            image_stub, result_ref_path=("result", "sample")
        )
        image_model = ModelInfo(
            id="stub/image-1",
            label="Stub Image",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            output_kind="image",
        )
        with patch.dict(
            providers.ASYNC_JOB_DESCRIPTORS, {"stub": image_desc}
        ), patch.object(providers.keys, "lookup_key", return_value="secret-key"):
            job = manager.submit(image_model, "a lighthouse", 1024, 1024, 7)
            final = self._wait_job(job)
        self.assertEqual(final.state, "done")
        self.assertTrue(final.result_path.endswith(".png"))
        with open(final.result_path, "rb") as handle:
            self.assertTrue(handle.read().startswith(PNG_SIGNATURE))

    def test_result_cap_is_per_row(self):
        row_cap = 8192
        admitted_body = STUB_MP4 + b"\x00" * (4096 - len(STUB_MP4))
        oversized_body = STUB_MP4 + b"\x00" * (row_cap + 4096)
        model = ModelInfo(
            id="stub/video-1",
            label="Stub Video",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            output_kind="video",
        )

        # A body larger than the image default cap but within the widened row
        # cap is admitted (the image default is shrunk here so the boundary is
        # exercised without moving megabytes).
        admit_stub = StubAsyncVendor(
            mode="ready",
            polls_before_ready=1,
            result_kind="video",
            video_body=admitted_body,
        ).start()
        self.addCleanup(admit_stub.stop)
        admit_desc = self._stub_async_descriptor(
            admit_stub, result_max_bytes=row_cap
        )
        with patch.object(providers, "MAX_INPUT_FILE_BYTES", 1024):
            result, _ = self._run_async_direct(admit_desc, model, "cap-ok.mp4")
        self.assertGreater(len(admitted_body), 1024)
        with open(result.path, "rb") as handle:
            self.assertEqual(len(handle.read()), len(admitted_body))

        # A body past the row cap is refused with a generic error and no key.
        refuse_stub = StubAsyncVendor(
            mode="ready",
            polls_before_ready=1,
            result_kind="video",
            video_body=oversized_body,
        ).start()
        self.addCleanup(refuse_stub.stop)
        refuse_desc = self._stub_async_descriptor(
            refuse_stub, result_max_bytes=row_cap
        )
        with self.assertRaises(RuntimeError) as caught:
            self._run_async_direct(refuse_desc, model, "cap-refused.mp4")
        self.assertIn("too large", str(caught.exception))
        self.assertNotIn("secret-key", str(caught.exception))

    def test_structured_body_hook_builds_nested_body(self):
        def content_builder(desc, request):
            body = {
                "content": [{"type": "text", "text": request.prompt}],
                "model": providers._slug(request.model),
            }
            return body, request.width, request.height

        stub = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(stub, build_body=content_builder)
        model = ModelInfo(
            id="stub/video-1",
            label="Stub Video",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            model_slug="nested-slug",
            output_kind="video",
        )
        self._run_async_direct(desc, model, "nested-body.mp4")
        self.assertEqual(
            stub.submit_body["content"][0]["text"], "a lighthouse at dusk"
        )
        self.assertEqual(stub.submit_body["model"], "nested-slug")
        self.assertNotIn("prompt", stub.submit_body)

    def test_input_image_encoded_into_submit_body(self):
        input_path = self.render_input(name="i2v-input.png")
        with open(input_path, "rb") as handle:
            input_bytes = handle.read()

        stub = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(
            stub, input_body_key="promptImage", input_encoding="data_uri"
        )
        model = ModelInfo(
            id="stub/video-1",
            label="Stub Video",
            provider="stub",
            price_usd=0.1,
            needs_key=True,
            needs_input=True,
            output_kind="video",
        )
        self._run_async_direct(desc, model, "i2v.mp4", input_path=input_path)
        encoded = stub.submit_body["promptImage"]
        self.assertTrue(encoded.startswith("data:image/png;base64,"))
        decoded = base64.b64decode(encoded.split(",", 1)[1])
        self.assertEqual(decoded, input_bytes)

        # A text-to-video row (no input field) carries no encoded image.
        t2v_stub = StubAsyncVendor(
            mode="ready", polls_before_ready=1, result_kind="video"
        ).start()
        self.addCleanup(t2v_stub.stop)
        t2v_desc = self._stub_async_descriptor(t2v_stub)
        self._run_async_direct(t2v_desc, model, "t2v.mp4", input_path=input_path)
        self.assertNotIn("promptImage", t2v_stub.submit_body)

    def test_stability_http_code_status_and_bytes_result(self):
        input_path = self.render_input(name="stability-input.png")
        stub = StubAsyncVendor(mode="stability", polls_before_ready=2).start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(
            stub,
            body_encoding="multipart",
            status_source="http_code",
            result_fetch="bytes",
            input_body_key="image",
            submit_headers={"accept": "video/*"},
            extra_body={"cfg_scale": 1.8, "motion_bucket_id": 127},
            poll_url_path=None,
            poll_path="/poll/{job_id}",
        )
        model = ModelInfo(
            id="stub/stability",
            label="Stub Stability",
            provider="stub",
            price_usd=0.2,
            needs_key=True,
            needs_input=True,
            output_kind="video",
        )
        result, progress = self._run_async_direct(
            desc, model, "stability.mp4", input_path=input_path
        )
        # The submit was multipart carrying the image part and the accept header.
        self.assertIn(
            "multipart/form-data", stub.submit_headers.get("Content-Type", "")
        )
        self.assertEqual(stub.submit_headers.get("accept"), "video/*")
        self.assertIn(b'name="image"', stub.submit_raw)
        self.assertIn(b"cfg_scale", stub.submit_raw)
        # The key rode submit and poll but never a separate result fetch.
        self.assertEqual(stub.submit_headers.get("x-key"), "secret-key")
        self.assertEqual(stub.poll_headers.get("x-key"), "secret-key")
        # 202 kept polling; 200 was terminal and its body is the mp4.
        self.assertGreater(stub.poll_count, 2)
        with open(result.path, "rb") as handle:
            self.assertEqual(handle.read()[4:8], MP4_FTYP)
        self.assertEqual(progress, sorted(progress))
        self.assertTrue(all(value < 1.0 for value in progress))

    def test_stability_http_code_terminal_error_fails_promptly(self):
        input_path = self.render_input(name="stability-error-input.png")
        stub = StubAsyncVendor(mode="stability", error_status=400).start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(
            stub,
            body_encoding="multipart",
            status_source="http_code",
            result_fetch="bytes",
            input_body_key="image",
            submit_headers={"accept": "video/*"},
            extra_body={"cfg_scale": 1.8, "motion_bucket_id": 127},
            poll_url_path=None,
            poll_path="/poll/{job_id}",
            job_deadline_s=5.0,
        )
        model = ModelInfo(
            id="stub/stability",
            label="Stub Stability",
            provider="stub",
            price_usd=0.2,
            needs_key=True,
            needs_input=True,
            output_kind="video",
        )
        started = time.monotonic()
        with self.assertRaises(RuntimeError) as caught:
            self._run_async_direct(
                desc, model, "stability-error.mp4", input_path=input_path
            )
        elapsed = time.monotonic() - started
        # A terminal 4xx settles the job at once rather than waiting out the
        # whole video deadline.
        self.assertLess(elapsed, desc.job_deadline_s / 2)
        message = str(caught.exception)
        self.assertIn("400", message)
        # The failure message carries only the status code, never the key or a
        # host.
        self.assertNotIn("secret-key", message)
        self.assertNotIn("127.0.0.1", message)
        self.assertNotIn(str(stub.port), message)

    def test_stability_submit_ssrf_guarded(self):
        # The multipart submit runs through the same SSRF gate as the JSON
        # submit: a malicious submit host is refused before connecting.
        input_path = self.render_input(name="submit-ssrf-input.png")
        desc = providers.AsyncJobDescriptor(
            provider="stub",
            base_url="https://169.254.169.254",
            auth_header="x-key",
            auth_template="{key}",
            body_encoding="multipart",
            status_source="http_code",
            result_fetch="bytes",
            input_body_key="image",
            submit_headers={"accept": "video/*"},
            extra_body={"cfg_scale": 1.8},
            success_states=("Ready",),
            poll_url_path=None,
            poll_path="/result/{job_id}",
            result_max_bytes=256 * 1024 * 1024,
            poll_interval_s=0.01,
            poll_backoff_ceiling_s=0.02,
            job_deadline_s=2.0,
        )
        model = ModelInfo(
            id="stub/stability",
            label="Stub Stability",
            provider="stub",
            price_usd=0.2,
            needs_key=True,
            needs_input=True,
            output_kind="video",
        )
        with self.assertRaises(RuntimeError) as caught:
            self._run_async_direct(desc, model, "submit-ssrf.mp4", input_path=input_path)
        self.assertNotIn("169.254", str(caught.exception))
        self.assertNotIn("secret-key", str(caught.exception))

    def test_stability_poll_url_ssrf_guarded(self):
        # A vendor-supplied poll URL aimed at the metadata host is refused on the
        # new http-code poll path with no key in the message.
        input_path = self.render_input(name="poll-ssrf-input.png")
        stub = StubAsyncVendor(mode="ssrf").start()
        self.addCleanup(stub.stop)
        desc = self._stub_async_descriptor(
            stub,
            body_encoding="multipart",
            status_source="http_code",
            result_fetch="bytes",
            input_body_key="image",
            submit_headers={"accept": "video/*"},
            extra_body={"cfg_scale": 1.8},
            poll_url_path=("polling_url",),
        )
        model = ModelInfo(
            id="stub/stability",
            label="Stub Stability",
            provider="stub",
            price_usd=0.2,
            needs_key=True,
            needs_input=True,
            output_kind="video",
        )
        with self.assertRaises(RuntimeError) as caught:
            self._run_async_direct(desc, model, "poll-ssrf.mp4", input_path=input_path)
        self.assertNotIn("169.254", str(caught.exception))
        self.assertNotIn("secret-key", str(caught.exception))

    def test_runway_video_row_drives_full_loop(self):
        self._drive_video_row(
            "runway/gen4-turbo", "runway", "RUNWAYML_API_SECRET", "gen4_turbo", True
        )

    def test_luma_video_row_drives_full_loop(self):
        self._drive_video_row(
            "luma/ray-3", "luma", "LUMA_API_KEY", "ray-3.2", False
        )

    def test_leonardo_video_row_drives_full_loop(self):
        self._drive_video_row(
            "leonardo/motion-2", "leonardo", "LEONARDO_API_KEY", "motion_2.0", False
        )

    def test_seedance_video_row_drives_full_loop(self):
        stub, _ = self._drive_video_row(
            "bytedance/seedance-1-pro",
            "seedance-video",
            "ARK_API_KEY",
            "seedance-1-0-pro-250528",
            False,
        )
        # The nested content body carries the prompt as a text part.
        self.assertEqual(
            stub.submit_body["content"][0]["text"], "a lighthouse at dusk"
        )

    def test_stability_video_row_drives_full_loop(self):
        self._drive_video_row(
            "stability/image-to-video",
            "stability",
            "STABILITY_API_KEY",
            "image-to-video",
            True,
        )

    def test_video_row_failure_surfaces_message_without_leaking_key(self):
        self.set_env_key("LUMA_API_KEY")
        stub = self._video_stub_for("luma", mode="failure").start()
        self.addCleanup(stub.stop)
        self.redirect_async_row("luma", stub, error_msg_path=("details",))
        final = self.run_to_done(
            model="luma/ray-3", prompt="x", width=1024, height=1024
        )
        self.assertEqual(final["state"], "error")
        self.assertIn("quota exceeded", final["message"])
        self.assertNotIn("test", final["message"])

    def test_video_row_deadline_trips_cleanly(self):
        self.set_env_key("LUMA_API_KEY")
        stub = self._video_stub_for("luma", mode="never").start()
        self.addCleanup(stub.stop)
        self.redirect_async_row("luma", stub, job_deadline_s=0.2)
        started = time.monotonic()
        final = self.run_to_done(
            model="luma/ray-3", prompt="x", width=1024, height=1024
        )
        elapsed = time.monotonic() - started
        self.assertEqual(final["state"], "error")
        self.assertLess(elapsed, 3.0)
        self.assertFalse(final["result_path"])
        self.assertNotIn("test", final["message"])

    def test_video_rows_are_excluded_from_models_yet_resolve(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        listed = {model["id"] for model in data["models"]}
        rows = (
            ("runway/gen4-turbo", "runway", True),
            ("luma/ray-3", "luma", False),
            ("leonardo/motion-2", "leonardo", False),
            ("bytedance/seedance-1-pro", "bytedance", False),
            ("stability/image-to-video", "stability", True),
        )
        for model_id, provider, needs_input in rows:
            self.assertNotIn(model_id, listed)
            overrides = {"model": model_id}
            if needs_input:
                overrides["input_path"] = self.render_input(
                    name=f"{provider}-resolve.png"
                )
            status, data = self.submit(**overrides)
            self.assertEqual(status, 409, model_id)
            self.assertEqual(data["error"], "missing_key")
            self.assertEqual(data["provider"], provider)

    def _assert_video_row_valid(self, model):
        desc = providers.ASYNC_JOB_DESCRIPTORS[model.descriptor or model.provider]
        # A video budget runs in minutes, not the still-image seconds.
        self.assertGreaterEqual(desc.job_deadline_s, 120.0)
        self.assertTrue(desc.result_ref_path or desc.result_fetch == "bytes")
        if model.needs_input:
            self.assertTrue(desc.input_body_key or desc.build_body is not None)

    def test_video_rows_are_well_formed(self):
        video_models = [m for m in list_models() if m.output_kind == "video"]
        self.assertTrue(video_models)
        for model in video_models:
            self._assert_video_row_valid(model)

        # A malformed video row (a still-image-scale deadline) fails the check.
        stunted = dataclasses.replace(
            providers.ASYNC_JOB_DESCRIPTORS["luma"], job_deadline_s=5.0
        )
        with patch.dict(providers.ASYNC_JOB_DESCRIPTORS, {"luma": stunted}):
            with self.assertRaises(AssertionError):
                self._assert_video_row_valid(model_by_id("luma/ray-3"))

    def test_url_download_refuses_internal_and_plaintext_hosts(self):
        for url in (
            "http://example.com/x.png",
            "https://169.254.169.254/latest/meta-data/",
            "https://10.0.0.1/x.png",
        ):
            with self.assertRaises(RuntimeError) as caught:
                providers._download_capped(
                    url, providers.MAX_INPUT_FILE_BYTES, timeout=5
                )
            self.assertNotIn("test", str(caught.exception))

        # A loopback http URL is allowed through the gate and served locally.
        stub = StubVendor()
        stub.start()
        self.addCleanup(stub.stop)
        data = providers._download_capped(
            stub.url + "/img/x.png", providers.MAX_INPUT_FILE_BYTES, timeout=5
        )
        self.assertTrue(data.startswith(PNG_SIGNATURE))

    def test_vendor_loopback_http_url_is_refused_in_production(self):
        # With the test-only loopback carve-out off (production posture), a
        # vendor-supplied http://127.0.0.1 URL must be refused like any other
        # internal host — a compromised vendor cannot reach a local service.
        stub = StubVendor()
        stub.start()
        self.addCleanup(stub.stop)
        loopback_url = stub.url + "/img/x.png"
        with patch.object(providers, "_ALLOW_LOOPBACK_HTTP", False):
            with self.assertRaises(RuntimeError) as caught:
                providers._download_capped(
                    loopback_url, providers.MAX_INPUT_FILE_BYTES, timeout=5
                )
        self.assertNotIn("127.0.0.1", str(caught.exception))

        # With the transport carve-out on, the same URL is served locally so
        # the in-process stub matrix keeps working.
        data = providers._download_capped(
            loopback_url, providers.MAX_INPUT_FILE_BYTES, timeout=5
        )
        self.assertTrue(data.startswith(PNG_SIGNATURE))

    def test_url_download_refuses_a_redirect_to_an_internal_host(self):
        # A host that passes the gate but 302s to the cloud-metadata endpoint
        # must have the redirect target re-validated and refused, not followed.
        stub = StubVendor(
            redirect_location="http://169.254.169.254/latest/meta-data/"
        )
        stub.start()
        self.addCleanup(stub.stop)
        with self.assertRaises(RuntimeError) as caught:
            providers._download_capped(
                stub.url + "/img/x.png", providers.MAX_INPUT_FILE_BYTES, timeout=5
            )
        self.assertNotIn("meta-data", str(caught.exception))

    def test_url_download_pins_the_validated_address(self):
        # The address validated by the guard must be the exact address the
        # transport connects to: no second, unvalidated resolution may run
        # between the check and the connect (the DNS-rebinding window).
        validated_ip = "93.184.216.34"

        class _ConnectReached(Exception):
            pass

        connected = []

        def fake_getaddrinfo(host, port=None, *args, **kwargs):
            try:
                ipaddress.ip_address(host)
                family = (
                    socket.AF_INET6 if ":" in host else socket.AF_INET
                )
                return [(family, socket.SOCK_STREAM, 6, "", (host, port or 0))]
            except ValueError:
                pass
            return [
                (
                    socket.AF_INET,
                    socket.SOCK_STREAM,
                    6,
                    "",
                    (validated_ip, port or 0),
                )
            ]

        def fake_create_connection(address, *args, **kwargs):
            connected.append(address[0])
            raise _ConnectReached()

        with patch("socket.getaddrinfo", side_effect=fake_getaddrinfo), patch(
            "socket.create_connection", side_effect=fake_create_connection
        ):
            with self.assertRaises(_ConnectReached):
                providers._download_capped(
                    "https://rebind.example/x.png",
                    providers.MAX_INPUT_FILE_BYTES,
                    timeout=5,
                )
        self.assertEqual(connected, [validated_ip])

    def test_url_download_refuses_ipv4_mapped_internal_address(self):
        # An IPv4-mapped IPv6 literal must be classified by its embedded IPv4
        # regardless of interpreter version.
        def fake_getaddrinfo(host, port=None, *args, **kwargs):
            return [
                (
                    socket.AF_INET6,
                    socket.SOCK_STREAM,
                    6,
                    "",
                    ("::ffff:169.254.169.254", port or 0, 0, 0),
                )
            ]

        with patch("socket.getaddrinfo", side_effect=fake_getaddrinfo):
            with self.assertRaises(RuntimeError):
                providers._download_capped(
                    "https://mapped.example/x.png",
                    providers.MAX_INPUT_FILE_BYTES,
                    timeout=5,
                )

    def _assert_descriptor_row_valid(self, provider, desc):
        self.assertEqual(desc.provider, provider)
        self.assertTrue(desc.base_url)
        self.assertTrue(desc.auth_header)
        self.assertTrue(desc.auth_template)
        self.assertTrue(desc.result_ref)
        self.assertIn(desc.result_fetch, {"inline_b64", "url"})
        self.assertIn(desc.size_mode, {"fixed_set", "passthrough"})

    def _assert_async_descriptor_row_valid(self, key, desc):
        self.assertTrue(desc.provider)
        self.assertTrue(desc.base_url)
        self.assertTrue(desc.auth_header)
        self.assertTrue(desc.auth_template)
        self.assertTrue(desc.submit_path)
        self.assertTrue(desc.job_id_path)
        self.assertTrue(desc.status_path)
        # A json-field row settles on a named success state; an http-code row
        # settles on the status code instead.
        self.assertTrue(desc.success_states or desc.status_source == "http_code")
        # A row either points at a result reference or returns the bytes inline.
        self.assertTrue(desc.result_ref_path or desc.result_fetch == "bytes")
        self.assertIn(desc.result_fetch, {"url", "inline_b64", "bytes"})

    def test_async_descriptor_rows_are_well_formed(self):
        for provider, desc in providers.ASYNC_JOB_DESCRIPTORS.items():
            self._assert_async_descriptor_row_valid(provider, desc)

        malformed = dataclasses.replace(
            providers.ASYNC_JOB_DESCRIPTORS["bfl"], success_states=()
        )
        with self.assertRaises(AssertionError):
            self._assert_async_descriptor_row_valid("bfl", malformed)

    def test_async_failure_state_becomes_error_without_leaking_key(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="failure").start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(stub)

        final = self.run_to_done(
            model="bfl/flux-2-pro-preview", prompt="x", width=1024, height=1024
        )
        self.assertEqual(final["state"], "error")
        self.assertIn("quota exceeded", final["message"])
        self.assertNotIn("test", final["message"])

    def test_async_deadline_trips_cleanly(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="never").start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(stub, job_deadline_s=0.2)

        started = time.monotonic()
        final = self.run_to_done(
            model="bfl/flux-2-pro-preview", prompt="x", width=1024, height=1024
        )
        elapsed = time.monotonic() - started
        self.assertEqual(final["state"], "error")
        self.assertLess(elapsed, 3.0)
        self.assertFalse(final["result_path"])
        self.assertNotIn("test", final["message"])

    def test_async_cancel_between_polls_fires_cancel_and_settles_canceled(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="cancel").start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(
            stub, cancel_url_path=("cancel_url",), job_deadline_s=5.0
        )

        _, submitted = self.submit(
            model="bfl/flux-2-pro-preview", prompt="x", width=1024, height=1024
        )
        job_id = submitted["job_id"]
        canceled = []

        def cancel_after_a_poll(snapshot):
            if not canceled and stub.poll_count >= 1:
                status, data = self.request("POST", f"/jobs/{job_id}/cancel")
                self.assertEqual(status, 200)
                canceled.append(True)
            return True

        snapshots = self.stream_events(job_id, cancel_after_a_poll)
        self.assertTrue(stub.cancel_hit)
        self.assertEqual(snapshots[-1]["state"], "canceled")
        self.assertEqual(snapshots[-1]["message"], "Stopped")
        self.assertNotIn("done", [snap["state"] for snap in snapshots])

        # A descriptor with no cancel endpoint cancels cooperatively: the job
        # still settles canceled and no cancel route is hit.
        coop_stub = StubAsyncVendor(mode="cancel").start()
        self.addCleanup(coop_stub.stop)
        self.redirect_async_descriptor(
            coop_stub, cancel_url_path=None, job_deadline_s=5.0
        )
        _, submitted2 = self.submit(
            model="bfl/flux-2-pro-preview", prompt="y", width=1024, height=1024
        )
        job_id2 = submitted2["job_id"]
        canceled2 = []

        def cancel_coop(snapshot):
            if not canceled2 and coop_stub.poll_count >= 1:
                self.request("POST", f"/jobs/{job_id2}/cancel")
                canceled2.append(True)
            return True

        snapshots2 = self.stream_events(job_id2, cancel_coop)
        self.assertFalse(coop_stub.cancel_hit)
        self.assertEqual(snapshots2[-1]["state"], "canceled")
        self.assertEqual(snapshots2[-1]["message"], "Stopped")

    def test_async_progress_is_monotonic_and_sub_one(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="ready", polls_before_ready=4).start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(stub)

        progress = []
        _, submitted = self.submit(
            model="bfl/flux-2-pro-preview", prompt="x", width=512, height=512
        )
        snapshots = self.stream_events(
            submitted["job_id"],
            lambda snap: progress.append(snap["progress"]) or True,
        )
        self.assertEqual(snapshots[-1]["state"], "done")
        self.assertEqual(progress, sorted(progress))
        pre_terminal = [
            snap["progress"] for snap in snapshots if snap["state"] != "done"
        ]
        self.assertTrue(all(value < 1.0 for value in pre_terminal))
        self.assertEqual(snapshots[-1]["progress"], 1.0)

    def test_async_poll_url_ssrf_guarded(self):
        self.set_env_key("BFL_API_KEY")
        stub = StubAsyncVendor(mode="ssrf").start()
        self.addCleanup(stub.stop)
        self.redirect_async_descriptor(stub)

        final = self.run_to_done(
            model="bfl/flux-2-pro-preview", prompt="x", width=1024, height=1024
        )
        self.assertEqual(final["state"], "error")
        self.assertNotIn("169.254", final["message"])
        self.assertNotIn("test", final["message"])

    def test_descriptor_table_rows_are_well_formed(self):
        for provider, desc in providers.SYNC_IMAGE_DESCRIPTORS.items():
            self._assert_descriptor_row_valid(provider, desc)

        malformed = dataclasses.replace(
            providers.SYNC_IMAGE_DESCRIPTORS["openai"], base_url=""
        )
        with self.assertRaises(AssertionError):
            self._assert_descriptor_row_valid("openai", malformed)

    def test_models_report_prompt_and_input_needs(self):
        status, data = self.request("GET", "/models")
        self.assertEqual(status, 200)
        by_id = {model["id"]: model for model in data["models"]}
        upscale = by_id["local/procedural-upscale-v1"]
        self.assertFalse(upscale["needs_prompt"])
        self.assertTrue(upscale["needs_input"])
        self.assertTrue(upscale["has_key"])
        edit = by_id["local/procedural-edit-v1"]
        self.assertTrue(edit["needs_prompt"])
        self.assertTrue(edit["needs_input"])
        base = by_id["local/procedural-v1"]
        self.assertTrue(base["needs_prompt"])
        self.assertFalse(base["needs_input"])

    def test_result_digest_matches_the_file(self):
        final = self.run_to_done()
        with open(final["result_path"], "rb") as handle:
            content = handle.read()
        self.assertEqual(
            final["result_digest"], hashlib.sha256(content).hexdigest()
        )

    def test_upscale_doubles_the_input_and_is_deterministic(self):
        input_path = self.render_input()
        finals = [
            self.run_to_done(
                model="local/procedural-upscale-v1",
                prompt="",
                input_path=input_path,
            )
            for _ in range(2)
        ]
        for final in finals:
            self.assertEqual(final["state"], "done")
            self.assertEqual((final["width"], final["height"]), (192, 128))
        self.assertEqual(finals[0]["result_digest"], finals[1]["result_digest"])

        # The output really is the input scaled: corner pixels survive.
        with open(input_path, "rb") as handle:
            in_w, in_h, in_rows = decode_png(handle.read())
        with open(finals[0]["result_path"], "rb") as handle:
            out_w, out_h, out_rows = decode_png(handle.read())
        self.assertEqual((out_w, out_h), (in_w * 2, in_h * 2))
        self.assertEqual(out_rows[0][:3], in_rows[0][:3])
        self.assertEqual(out_rows[-1][-3:], in_rows[-1][-3:])

    def test_edit_blends_over_the_input(self):
        input_path = self.render_input(name="edit-input.png")
        base = self.run_to_done(
            model="local/procedural-edit-v1",
            prompt="warm dusk grade",
            input_path=input_path,
        )
        self.assertEqual(base["state"], "done")
        self.assertEqual((base["width"], base["height"]), (96, 64))

        with open(input_path, "rb") as handle:
            input_digest = hashlib.sha256(handle.read()).hexdigest()
        self.assertNotEqual(base["result_digest"], input_digest)

        again = self.run_to_done(
            model="local/procedural-edit-v1",
            prompt="warm dusk grade",
            input_path=input_path,
        )
        self.assertEqual(again["result_digest"], base["result_digest"])

        different = self.run_to_done(
            model="local/procedural-edit-v1",
            prompt="cold morning grade",
            input_path=input_path,
        )
        self.assertNotEqual(different["result_digest"], base["result_digest"])

    def test_input_only_model_accepts_an_empty_prompt(self):
        input_path = self.render_input(name="promptless.png")
        status, submitted = self.submit(
            model="local/procedural-upscale-v1", prompt="", input_path=input_path
        )
        self.assertEqual(status, 202)
        snapshots = self.stream_events(submitted["job_id"], lambda snap: True)
        self.assertEqual(snapshots[-1]["state"], "done")

    def test_missing_input_is_rejected(self):
        for input_path in (None, os.path.join(self.gen_dir, "absent.png")):
            overrides = {"model": "local/procedural-upscale-v1", "prompt": ""}
            if input_path is not None:
                overrides["input_path"] = input_path
            status, data = self.submit(**overrides)
            self.assertEqual(status, 400)
            self.assertEqual(data["error"], "missing_input")

    def test_unsupported_input_surfaces_a_job_error(self):
        junk_path = os.path.join(self.gen_dir, "junk.bin")
        with open(junk_path, "wb") as handle:
            handle.write(b"not a png at all")
        final = self.run_to_done(
            model="local/procedural-upscale-v1", prompt="", input_path=junk_path
        )
        self.assertEqual(final["state"], "error")
        self.assertIn("PNG", final["message"])

    def test_decode_png_refuses_a_decompression_bomb(self):
        # A tiny file declaring an 8x8 image but hiding a 128 MB IDAT payload
        # must be refused without expanding it — the decoder decompresses no
        # more than the declared image would occupy.
        bomb = _png_with_idat(8, 8, b"\x00" * (128 * 1024 * 1024))
        self.assertLess(len(bomb), 512 * 1024)

        tracemalloc.start()
        try:
            with self.assertRaises(ValueError):
                decode_png(bomb)
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        # A real 8x8 image needs a couple hundred bytes; the bomb's payload is
        # 128 MB. A generous ceiling proves the expansion never happened.
        self.assertLess(peak, 16 * 1024 * 1024)

    def test_decode_png_refuses_oversized_dimensions(self):
        oversize = MAX_INPUT_DIMENSION + 1
        # A one-row payload keeps the file tiny; the dimensions are rejected
        # from the header before any decompression is attempted.
        png = _png_with_idat(oversize, 1, b"\x00" * (oversize * 3 + 1))
        with self.assertRaises(ValueError):
            decode_png(png)

    def test_input_bomb_surfaces_a_job_error_not_a_crash(self):
        bomb_path = os.path.join(self.gen_dir, "bomb.png")
        with open(bomb_path, "wb") as handle:
            handle.write(_png_with_idat(8, 8, b"\x00" * (128 * 1024 * 1024)))
        final = self.run_to_done(
            model="local/procedural-upscale-v1", prompt="", input_path=bomb_path
        )
        self.assertEqual(final["state"], "error")


if __name__ == "__main__":
    unittest.main()
