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
import tracemalloc
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from generation_sidecar import providers  # noqa: E402
from generation_sidecar.procedural import (  # noqa: E402
    MAX_INPUT_DIMENSION,
    decode_png,
    render_png,
)
from generation_sidecar.registry import model_by_id  # noqa: E402
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
