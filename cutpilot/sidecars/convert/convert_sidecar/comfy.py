"""ComfyUI workflow mapping.

Imports the ComfyUI editor's workflow JSON ({"nodes": [...], "links":
[...]}) into canvas node specs, grading every node into a tier:

  exact        the registry maps the type 1:1 onto a canvas node kind and
               the payload carries over losslessly
  substituted  the nearest canvas kind stands in; parameters are
               approximated
  passthrough  the type is unknown; the node is preserved whole — its
               original JSON rides along and the card displays the type
  unresolved   the entry is malformed or its links point at missing nodes;
               preserved whole and flagged

Nothing is dropped: every input node yields exactly one output spec and one
report row. Export runs the registry backwards; preserved foreign nodes
re-emit their original JSON untouched.
"""

from __future__ import annotations

import json

EXACT = {
    "CLIPTextEncode": "prompt",
    "LoadImage": "still",
}

SUBSTITUTED = {
    "KSampler": "generate",
    "KSamplerAdvanced": "generate",
    "ImageBlend": "blend",
    "ImageCompositeMasked": "mask",
    "ImageScale": "transform",
    "ImageScaleBy": "transform",
}

_EXPORT_TYPES = {
    "prompt": "CLIPTextEncode",
    "still": "LoadImage",
    "video": "LoadImage",
    "generate": "KSampler",
    "blend": "ImageBlend",
    "mask": "ImageCompositeMasked",
    "key": "ImageCompositeMasked",
    "transform": "ImageScale",
}


class WorkflowError(ValueError):
    pass


def _node_position(entry: dict) -> list[float]:
    pos = entry.get("pos")
    if isinstance(pos, (list, tuple)) and len(pos) >= 2:
        try:
            return [float(pos[0]), float(pos[1])]
        except (TypeError, ValueError):
            return [0.0, 0.0]
    if isinstance(pos, dict):
        try:
            return [float(pos.get("0", 0)), float(pos.get("1", 0))]
        except (TypeError, ValueError):
            return [0.0, 0.0]
    return [0.0, 0.0]


def _first_string_widget(entry: dict) -> str:
    values = entry.get("widgets_values")
    if isinstance(values, list):
        for value in values:
            if isinstance(value, str):
                return value
    return ""


def import_workflow(workflow: dict) -> dict:
    if not isinstance(workflow, dict) or not isinstance(
        workflow.get("nodes"), list
    ):
        raise WorkflowError("workflow must carry a nodes list")

    raw_nodes = workflow["nodes"]
    raw_links = workflow.get("links")
    if raw_links is None:
        raw_links = []
    if not isinstance(raw_links, list):
        raise WorkflowError("workflow links must be a list")

    known_ids = set()
    for entry in raw_nodes:
        if isinstance(entry, dict) and isinstance(entry.get("id"), int):
            known_ids.add(entry["id"])

    # A link whose endpoints both exist becomes a connection; a dangling one
    # taints the surviving endpoint, whose wiring can no longer be trusted.
    connections: list[dict] = []
    tainted: set[int] = set()
    for link in raw_links:
        if not isinstance(link, (list, tuple)) or len(link) < 5:
            continue
        from_id, to_id = link[1], link[3]
        from_known = from_id in known_ids
        to_known = to_id in known_ids
        if from_known and to_known:
            connections.append({"from": from_id, "to": to_id})
        elif from_known:
            tainted.add(from_id)
        elif to_known:
            tainted.add(to_id)

    nodes: list[dict] = []
    report: list[dict] = []
    for index, entry in enumerate(raw_nodes):
        malformed = (
            not isinstance(entry, dict)
            or not isinstance(entry.get("id"), int)
            or not isinstance(entry.get("type"), str)
            or not entry.get("type")
        )
        entry_dict = entry if isinstance(entry, dict) else {}
        comfy_id = entry_dict.get("id") if isinstance(
            entry_dict.get("id"), int
        ) else -(index + 1)
        comfy_type = str(entry_dict.get("type") or "")

        if malformed or comfy_id in tainted:
            tier = "unresolved"
            kind = "blank"
        elif comfy_type in EXACT:
            tier = "exact"
            kind = EXACT[comfy_type]
        elif comfy_type in SUBSTITUTED:
            tier = "substituted"
            kind = SUBSTITUTED[comfy_type]
        else:
            tier = "passthrough"
            kind = "blank"

        spec = {
            "comfy_id": comfy_id,
            "comfy_type": comfy_type,
            "tier": tier,
            "kind": kind,
            "title": str(entry_dict.get("title") or comfy_type or "Unknown"),
            "pos": _node_position(entry_dict),
            "prompt": "",
            "media": "",
            "opaque": None,
        }
        if kind == "prompt":
            spec["prompt"] = _first_string_widget(entry_dict)
        elif kind == "still":
            spec["media"] = _first_string_widget(entry_dict)
        if tier in ("passthrough", "unresolved"):
            spec["opaque"] = entry if isinstance(entry, dict) else {
                "malformed": json.loads(json.dumps(entry, default=str))
            }

        nodes.append(spec)
        report.append(
            {
                "id": comfy_id,
                "type": comfy_type or "(missing)",
                "tier": tier,
                "mapped": kind,
            }
        )

    return {"nodes": nodes, "connections": connections, "report": report}


def export_workflow(nodes: list, connections: list) -> dict:
    if not isinstance(nodes, list) or not isinstance(connections, list):
        raise WorkflowError("nodes and connections must be lists")

    out_nodes: list[dict] = []
    id_map: dict = {}
    next_id = 1
    for entry in nodes:
        if not isinstance(entry, dict):
            raise WorkflowError("every node must be an object")
        opaque = entry.get("opaque")
        if isinstance(opaque, dict) and opaque:
            preserved = dict(opaque)
            preserved.setdefault("id", next_id)
            id_map[entry.get("ref", next_id)] = preserved["id"]
            out_nodes.append(preserved)
            next_id = max(next_id, int(preserved["id"])) + 1
            continue

        kind = str(entry.get("kind") or "blank")
        comfy = {
            "id": next_id,
            "type": _EXPORT_TYPES.get(kind, "Note"),
            "pos": entry.get("pos") or [0, 0],
            "widgets_values": [],
        }
        if kind == "prompt":
            comfy["widgets_values"] = [str(entry.get("prompt") or "")]
        elif kind in ("still", "video"):
            comfy["widgets_values"] = [str(entry.get("media") or "")]
        id_map[entry.get("ref", next_id)] = next_id
        out_nodes.append(comfy)
        next_id += 1

    out_links: list[list] = []
    for index, connection in enumerate(connections):
        if not isinstance(connection, dict):
            continue
        from_ref = connection.get("from")
        to_ref = connection.get("to")
        if from_ref in id_map and to_ref in id_map:
            out_links.append(
                [index + 1, id_map[from_ref], 0, id_map[to_ref], 0, "*"]
            )

    return {"nodes": out_nodes, "links": out_links, "version": 0.4}
