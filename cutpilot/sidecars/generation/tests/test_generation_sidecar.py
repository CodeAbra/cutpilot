"""End-to-end tests for the generation sidecar over a live loopback server."""

import hashlib
import json
import http.client
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from generation_sidecar.server import build_server  # noqa: E402

TOKEN = "test-token"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


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


if __name__ == "__main__":
    unittest.main()
