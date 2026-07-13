"""End-to-end tests for the convert sidecar over a live loopback server.

The interchange writers are validated by re-parsing their output against
each format's grammar — element structure, reference integrity, rational
time syntax — not by substring checks. When the opentimelineio package is
importable the OTIO output is additionally round-tripped through it.
"""

from __future__ import annotations

import http.client
import importlib.util
import json
import os
import re
import sys
import tempfile
import threading
import unittest

# Parsed XML here is exclusively files this suite just wrote itself; no
# untrusted XML ever reaches this parser (the service itself speaks JSON).
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from convert_sidecar.server import build_server  # noqa: E402

TOKEN = "test-token"
RATIONAL_SECONDS = re.compile(r"^(0|\d+/\d+)s$")

HAVE_OTIO = importlib.util.find_spec("opentimelineio") is not None


def timeline_payload(out_path: str, media_dir: str) -> dict:
    first = os.path.join(media_dir, "shot-one.png")
    second = os.path.join(media_dir, "shot-two.png")
    for path in (first, second):
        with open(path, "wb") as handle:
            handle.write(b"\x89PNG\r\n\x1a\n")
    return {
        "title": "Canvas Cut",
        "fps": {"num": 24, "den": 1},
        "width": 1920,
        "height": 1080,
        "out_path": out_path,
        "segments": [
            {
                "name": "shot-one.png",
                "path": first,
                "timeline_in": 0,
                "timeline_out": 96,
                "source_in": 0,
                "source_out": 96,
                "generator": True,
            },
            # A hole before the second shot: writers must emit a gap.
            {
                "name": "shot-two.png",
                "path": second,
                "timeline_in": 120,
                "timeline_out": 168,
                "source_in": 0,
                "source_out": 48,
                "generator": False,
            },
        ],
    }


class ConvertTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = build_server(TOKEN, port=0)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(
            target=cls.server.serve_forever, daemon=True
        )
        cls.thread.start()
        cls.tmp = tempfile.TemporaryDirectory(prefix="cutpilot-convert-test-")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=5)
        cls.tmp.cleanup()

    def request(self, method, path, body=None, token=TOKEN):
        connection = http.client.HTTPConnection("127.0.0.1", self.port)
        headers = {}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        payload = None
        if body is not None:
            payload = json.dumps(body)
            headers["Content-Type"] = "application/json"
        connection.request(method, path, body=payload, headers=headers)
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, json.loads(data) if data else {}

    # -- authentication ---------------------------------------------------

    def test_rejects_missing_and_wrong_tokens(self):
        status, body = self.request("GET", "/health", token=None)
        self.assertEqual(status, 401)
        self.assertEqual(body["error"], "unauthorized")
        status, _ = self.request("GET", "/health", token="wrong")
        self.assertEqual(status, 401)
        status, _ = self.request(
            "POST", "/comfyui/import", body={"workflow": {}}, token="wrong"
        )
        self.assertEqual(status, 401)

    def test_health_reports_serving(self):
        status, body = self.request("GET", "/health")
        self.assertEqual(status, 200)
        self.assertEqual(body["status"], "serving")

    # -- FCPXML -----------------------------------------------------------

    def test_fcpxml_output_parses_against_the_grammar(self):
        out_path = os.path.join(self.tmp.name, "cut.fcpxml")
        payload = timeline_payload(out_path, self.tmp.name)
        status, body = self.request("POST", "/timeline/fcpxml", body=payload)
        self.assertEqual(status, 200)
        self.assertEqual(body["path"], out_path)

        with open(out_path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("<!DOCTYPE fcpxml>", text)

        root = ET.parse(out_path).getroot()
        self.assertEqual(root.tag, "fcpxml")
        self.assertEqual(root.get("version"), "1.11")

        resources = root.find("resources")
        self.assertIsNotNone(resources)
        fmt = resources.find("format")
        self.assertIsNotNone(fmt)
        self.assertRegex(fmt.get("frameDuration"), RATIONAL_SECONDS)
        self.assertEqual(fmt.get("width"), "1920")
        self.assertEqual(fmt.get("height"), "1080")

        assets = resources.findall("asset")
        self.assertEqual(len(assets), 2)
        asset_ids = set()
        for asset in assets:
            asset_ids.add(asset.get("id"))
            self.assertTrue(asset.get("name"))
            self.assertRegex(asset.get("duration"), RATIONAL_SECONDS)
            self.assertEqual(asset.get("hasVideo"), "1")
            self.assertEqual(asset.get("format"), fmt.get("id"))
            rep = asset.find("media-rep")
            self.assertIsNotNone(rep)
            self.assertEqual(rep.get("kind"), "original-media")
            self.assertTrue(rep.get("src").startswith("file://"))

        spine = root.find("library/event/project/sequence/spine")
        self.assertIsNotNone(spine)
        sequence = root.find("library/event/project/sequence")
        self.assertEqual(sequence.get("tcFormat"), "NDF")
        self.assertRegex(sequence.get("duration"), RATIONAL_SECONDS)

        children = list(spine)
        self.assertEqual([c.tag for c in children],
                         ["asset-clip", "gap", "asset-clip"])
        for child in children:
            self.assertRegex(child.get("offset"), RATIONAL_SECONDS)
            self.assertRegex(child.get("duration"), RATIONAL_SECONDS)
        for clip in spine.findall("asset-clip"):
            self.assertIn(clip.get("ref"), asset_ids)
            self.assertRegex(clip.get("start"), RATIONAL_SECONDS)

        # The gap covers exactly the hole: frames 96..120 at 24fps.
        gap = spine.find("gap")
        self.assertEqual(gap.get("offset"), "96/24s")
        self.assertEqual(gap.get("duration"), "24/24s")

    def test_fcpxml_dedupes_repeated_media_into_one_asset(self):
        out_path = os.path.join(self.tmp.name, "dedupe.fcpxml")
        payload = timeline_payload(out_path, self.tmp.name)
        payload["segments"][1]["path"] = payload["segments"][0]["path"]
        payload["segments"][1]["name"] = payload["segments"][0]["name"]
        status, _ = self.request("POST", "/timeline/fcpxml", body=payload)
        self.assertEqual(status, 200)
        root = ET.parse(out_path).getroot()
        assets = root.findall("resources/asset")
        self.assertEqual(len(assets), 1)
        clips = root.findall(".//spine/asset-clip")
        self.assertEqual(len(clips), 2)
        for clip in clips:
            self.assertEqual(clip.get("ref"), assets[0].get("id"))

    def test_timeline_payload_validation_refuses_broken_edits(self):
        out_path = os.path.join(self.tmp.name, "broken.fcpxml")
        payload = timeline_payload(out_path, self.tmp.name)
        payload["segments"][1]["timeline_in"] = 50  # overlaps the first shot
        status, body = self.request("POST", "/timeline/fcpxml", body=payload)
        self.assertEqual(status, 400)
        self.assertEqual(body["error"], "invalid_payload")

        payload = timeline_payload(out_path, self.tmp.name)
        payload["out_path"] = "relative/path.fcpxml"
        status, body = self.request("POST", "/timeline/fcpxml", body=payload)
        self.assertEqual(status, 400)

        payload = timeline_payload(out_path, self.tmp.name)
        payload["segments"][0]["timeline_out"] = 0
        status, body = self.request("POST", "/timeline/fcpxml", body=payload)
        self.assertEqual(status, 400)

    # -- OTIO ---------------------------------------------------------------

    def test_otio_output_matches_the_schema_shape(self):
        out_path = os.path.join(self.tmp.name, "cut.otio")
        payload = timeline_payload(out_path, self.tmp.name)
        status, body = self.request("POST", "/timeline/otio", body=payload)
        self.assertEqual(status, 200)
        self.assertEqual(body["path"], out_path)

        with open(out_path, encoding="utf-8") as handle:
            document = json.load(handle)

        self.assertEqual(document["OTIO_SCHEMA"], "Timeline.1")
        self.assertEqual(document["name"], "Canvas Cut")
        start = document["global_start_time"]
        self.assertEqual(start["OTIO_SCHEMA"], "RationalTime.1")
        self.assertEqual(start["rate"], 24.0)

        stack = document["tracks"]
        self.assertEqual(stack["OTIO_SCHEMA"], "Stack.1")
        tracks = stack["children"]
        self.assertEqual(len(tracks), 1)
        track = tracks[0]
        self.assertEqual(track["OTIO_SCHEMA"], "Track.1")
        self.assertEqual(track["kind"], "Video")

        children = track["children"]
        self.assertEqual(
            [c["OTIO_SCHEMA"] for c in children],
            ["Clip.2", "Gap.1", "Clip.2"],
        )
        for clip in (children[0], children[2]):
            rng = clip["source_range"]
            self.assertEqual(rng["OTIO_SCHEMA"], "TimeRange.1")
            for key in ("start_time", "duration"):
                self.assertEqual(rng[key]["OTIO_SCHEMA"], "RationalTime.1")
                self.assertIsInstance(rng[key]["value"], int)
                self.assertEqual(rng[key]["rate"], 24.0)
            key = clip["active_media_reference_key"]
            reference = clip["media_references"][key]
            self.assertEqual(reference["OTIO_SCHEMA"], "ExternalReference.1")
            self.assertTrue(reference["target_url"].startswith("file://"))

        self.assertTrue(children[0]["metadata"]["cutpilot"]["generator"])
        self.assertFalse(children[2]["metadata"]["cutpilot"]["generator"])
        gap = children[1]
        self.assertEqual(gap["source_range"]["duration"]["value"], 24)

    @unittest.skipUnless(HAVE_OTIO, "opentimelineio not installed")
    def test_otio_output_round_trips_through_opentimelineio(self):
        import opentimelineio as otio

        out_path = os.path.join(self.tmp.name, "roundtrip.otio")
        payload = timeline_payload(out_path, self.tmp.name)
        status, _ = self.request("POST", "/timeline/otio", body=payload)
        self.assertEqual(status, 200)
        timeline = otio.adapters.read_from_file(out_path)
        clips = list(timeline.find_clips())
        self.assertEqual(len(clips), 2)

    # -- ComfyUI ------------------------------------------------------------

    def comfy_workflow(self) -> dict:
        return {
            "nodes": [
                {
                    "id": 1,
                    "type": "CLIPTextEncode",
                    "pos": [80, 40],
                    "widgets_values": ["a lighthouse at dusk"],
                },
                {
                    "id": 2,
                    "type": "KSampler",
                    "pos": [400, 40],
                    "widgets_values": [42, "fixed", 20, 8.0],
                },
                {
                    "id": 3,
                    "type": "GlitterStorm",
                    "pos": [700, 40],
                    "widgets_values": [0.7],
                    "properties": {"sparkle": True},
                },
                {"id": 4, "pos": [900, 40]},  # no type: malformed
                {
                    "id": 5,
                    "type": "LoadImage",
                    "pos": [80, 300],
                    "widgets_values": ["backdrop.png"],
                },
            ],
            "links": [
                [1, 1, 0, 2, 1, "CONDITIONING"],
                [2, 5, 0, 2, 0, "IMAGE"],
                [3, 99, 0, 3, 0, "IMAGE"],  # from a missing node
            ],
        }

    def test_comfy_import_grades_every_node_and_drops_none(self):
        status, body = self.request(
            "POST", "/comfyui/import", body={"workflow": self.comfy_workflow()}
        )
        self.assertEqual(status, 200)

        report = {row["id"]: row for row in body["report"]}
        self.assertEqual(len(body["nodes"]), 5)
        self.assertEqual(len(report), 5)
        self.assertEqual(report[1]["tier"], "exact")
        self.assertEqual(report[1]["mapped"], "prompt")
        self.assertEqual(report[2]["tier"], "substituted")
        self.assertEqual(report[2]["mapped"], "generate")
        self.assertEqual(report[3]["tier"], "unresolved")  # dangling link
        self.assertEqual(report[4]["tier"], "unresolved")  # malformed
        self.assertEqual(report[5]["tier"], "exact")
        self.assertEqual(report[5]["mapped"], "still")

        nodes = {node["comfy_id"]: node for node in body["nodes"]}
        self.assertEqual(nodes[1]["prompt"], "a lighthouse at dusk")
        self.assertEqual(nodes[5]["media"], "backdrop.png")

        # The unknown node is preserved whole, original payload included.
        self.assertEqual(nodes[3]["kind"], "blank")
        self.assertEqual(nodes[3]["comfy_type"], "GlitterStorm")
        self.assertEqual(nodes[3]["opaque"]["properties"], {"sparkle": True})

        # Only links between surviving endpoints become connections.
        self.assertEqual(
            body["connections"],
            [{"from": 1, "to": 2}, {"from": 5, "to": 2}],
        )

    def test_comfy_import_of_unknown_node_survives_reexport(self):
        status, body = self.request(
            "POST", "/comfyui/import", body={"workflow": self.comfy_workflow()}
        )
        self.assertEqual(status, 200)
        preserved = next(
            node for node in body["nodes"] if node["comfy_type"] == "GlitterStorm"
        )
        status, exported = self.request(
            "POST",
            "/comfyui/export",
            body={
                "nodes": [
                    {"ref": 1, "kind": "prompt", "prompt": "dusk", "pos": [0, 0]},
                    {"ref": 3, "opaque": preserved["opaque"]},
                ],
                "connections": [{"from": 1, "to": 3}],
            },
        )
        self.assertEqual(status, 200)
        workflow = exported["workflow"]
        types = {node.get("type") for node in workflow["nodes"]}
        self.assertIn("CLIPTextEncode", types)
        self.assertIn("GlitterStorm", types)
        glitter = next(
            node for node in workflow["nodes"] if node.get("type") == "GlitterStorm"
        )
        self.assertEqual(glitter["properties"], {"sparkle": True})
        self.assertEqual(len(workflow["links"]), 1)

    def test_comfy_import_refuses_a_workflow_without_nodes(self):
        status, body = self.request(
            "POST", "/comfyui/import", body={"workflow": {"not": "nodes"}}
        )
        self.assertEqual(status, 400)
        self.assertEqual(body["error"], "invalid_workflow")

    # -- Resolve bridge -------------------------------------------------------

    def test_resolve_import_refuses_cleanly_without_resolve(self):
        os.environ["CUTPILOT_RESOLVE_MODULES"] = os.path.join(
            self.tmp.name, "no-such-modules"
        )
        try:
            missing = os.path.join(self.tmp.name, "missing.fcpxml")
            status, body = self.request(
                "POST", "/resolve/import", body={"path": missing}
            )
            self.assertEqual(status, 200)
            self.assertFalse(body["ok"])
            self.assertEqual(body["reason"], "file_not_found")

            existing = os.path.join(self.tmp.name, "exists.fcpxml")
            with open(existing, "w", encoding="utf-8") as handle:
                handle.write("<fcpxml/>")
            status, body = self.request(
                "POST", "/resolve/import", body={"path": existing}
            )
            self.assertEqual(status, 200)
            self.assertFalse(body["ok"])
            self.assertEqual(body["reason"], "resolve_unavailable")
        finally:
            os.environ.pop("CUTPILOT_RESOLVE_MODULES", None)


if __name__ == "__main__":
    unittest.main()
