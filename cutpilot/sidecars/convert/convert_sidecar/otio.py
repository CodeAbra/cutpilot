"""OpenTimelineIO JSON writer.

Emits the current OTIO document shape — Timeline.1 over a Stack.1 of
Track.1, clips as Clip.2 with a media_references table — without depending
on the opentimelineio package. Rates are the payload's exact ratio as a
float; positions and durations are integer frame values at that rate.
"""

from __future__ import annotations

import json
import os

from . import __version__
from .timeline_payload import TimelineSpec, file_uri


def _rational_time(value: int, rate: float) -> dict:
    return {"OTIO_SCHEMA": "RationalTime.1", "rate": rate, "value": value}


def _time_range(start: int, duration: int, rate: float) -> dict:
    return {
        "OTIO_SCHEMA": "TimeRange.1",
        "start_time": _rational_time(start, rate),
        "duration": _rational_time(duration, rate),
    }


def _clip(segment, rate: float) -> dict:
    duration = segment.timeline_out - segment.timeline_in
    return {
        "OTIO_SCHEMA": "Clip.2",
        "name": segment.name,
        "enabled": True,
        "source_range": _time_range(segment.source_in, duration, rate),
        "media_references": {
            "DEFAULT_MEDIA": {
                "OTIO_SCHEMA": "ExternalReference.1",
                "name": segment.name,
                "target_url": file_uri(segment.path),
                "available_range": _time_range(
                    0, segment.source_out, rate
                ),
                "available_image_bounds": None,
                "metadata": {},
            }
        },
        "active_media_reference_key": "DEFAULT_MEDIA",
        "effects": [],
        "markers": [],
        "metadata": {"cutpilot": {"generator": segment.generator}},
    }


def _gap(duration: int, rate: float) -> dict:
    return {
        "OTIO_SCHEMA": "Gap.1",
        "name": "Gap",
        "enabled": True,
        "source_range": _time_range(0, duration, rate),
        "effects": [],
        "markers": [],
        "metadata": {},
    }


def build_otio(spec: TimelineSpec) -> dict:
    rate = spec.fps_num / spec.fps_den
    children = []
    cursor = 0
    for segment in spec.segments:
        if segment.timeline_in > cursor:
            children.append(_gap(segment.timeline_in - cursor, rate))
        children.append(_clip(segment, rate))
        cursor = segment.timeline_out

    return {
        "OTIO_SCHEMA": "Timeline.1",
        "name": spec.title,
        "global_start_time": _rational_time(0, rate),
        "metadata": {"cutpilot": {"version": __version__}},
        "tracks": {
            "OTIO_SCHEMA": "Stack.1",
            "name": "tracks",
            "enabled": True,
            "source_range": None,
            "effects": [],
            "markers": [],
            "metadata": {},
            "children": [
                {
                    "OTIO_SCHEMA": "Track.1",
                    "name": "V1",
                    "kind": "Video",
                    "enabled": True,
                    "source_range": None,
                    "effects": [],
                    "markers": [],
                    "metadata": {},
                    "children": children,
                }
            ],
        },
    }


def write_otio(spec: TimelineSpec) -> str:
    document = build_otio(spec)
    os.makedirs(os.path.dirname(spec.out_path), exist_ok=True)
    with open(spec.out_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    return spec.out_path
