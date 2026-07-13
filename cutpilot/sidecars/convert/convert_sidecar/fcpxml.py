"""FCPXML 1.11 writer.

Times are exact rational seconds ("96/24s") at the payload's frame rate.
Each unique media file becomes one asset resource with an original-media
media-rep; shots become asset-clips on the sequence spine, and holes in the
timeline become explicit gap elements.
"""

from __future__ import annotations

import os
import xml.etree.ElementTree as ET

from .timeline_payload import TimelineSpec, file_uri

FCPXML_VERSION = "1.11"


def build_fcpxml(spec: TimelineSpec) -> ET.Element:
    root = ET.Element("fcpxml", version=FCPXML_VERSION)
    resources = ET.SubElement(root, "resources")
    ET.SubElement(
        resources,
        "format",
        id="r1",
        frameDuration=f"{spec.fps_den}/{spec.fps_num}s",
        width=str(spec.width),
        height=str(spec.height),
    )

    # One asset per unique file. Its declared duration must cover every
    # segment cut from it, whichever order the segments arrive in.
    asset_frames: dict[str, int] = {}
    for segment in spec.segments:
        asset_frames[segment.path] = max(
            asset_frames.get(segment.path, 0), segment.source_out
        )

    asset_ids: dict[str, str] = {}
    for segment in spec.segments:
        if segment.path in asset_ids:
            continue
        asset_id = f"r{len(asset_ids) + 2}"
        asset_ids[segment.path] = asset_id
        asset = ET.SubElement(
            resources,
            "asset",
            id=asset_id,
            name=segment.name,
            start="0s",
            duration=spec.frames_to_seconds(asset_frames[segment.path]),
            hasVideo="1",
            format="r1",
        )
        ET.SubElement(
            asset,
            "media-rep",
            kind="original-media",
            src=file_uri(segment.path),
        )

    library = ET.SubElement(root, "library")
    event = ET.SubElement(library, "event", name="CutPilot")
    project = ET.SubElement(event, "project", name=spec.title)
    duration = spec.segments[-1].timeline_out if spec.segments else 0
    sequence = ET.SubElement(
        project,
        "sequence",
        format="r1",
        duration=spec.frames_to_seconds(duration),
        tcStart="0s",
        tcFormat="NDF",
    )
    spine = ET.SubElement(sequence, "spine")

    cursor = 0
    for segment in spec.segments:
        if segment.timeline_in > cursor:
            ET.SubElement(
                spine,
                "gap",
                name="Gap",
                offset=spec.frames_to_seconds(cursor),
                duration=spec.frames_to_seconds(segment.timeline_in - cursor),
            )
        ET.SubElement(
            spine,
            "asset-clip",
            name=segment.name,
            ref=asset_ids[segment.path],
            offset=spec.frames_to_seconds(segment.timeline_in),
            start=spec.frames_to_seconds(segment.source_in),
            duration=spec.frames_to_seconds(
                segment.timeline_out - segment.timeline_in
            ),
            format="r1",
        )
        cursor = segment.timeline_out

    return root


def write_fcpxml(spec: TimelineSpec) -> str:
    root = build_fcpxml(spec)
    ET.indent(root)
    body = ET.tostring(root, encoding="unicode")
    document = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE fcpxml>\n'
        + body
        + "\n"
    )
    os.makedirs(os.path.dirname(spec.out_path), exist_ok=True)
    with open(spec.out_path, "w", encoding="utf-8") as handle:
        handle.write(document)
    return spec.out_path
