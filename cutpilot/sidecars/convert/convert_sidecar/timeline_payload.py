"""The timeline payload both writers consume, validated once at the door.

A payload is a JSON object:

  title      display name of the exported cut
  fps        {"num": int > 0, "den": int > 0} — exact rational frame rate
  width      pixels, > 0
  height     pixels, > 0
  segments   list of shots in timeline order; each carries
               name          display/file name of the media
               path          absolute path of the media file
               timeline_in   frame the shot starts at on the timeline
               timeline_out  frame it ends at (exclusive), > timeline_in
               source_in     first source frame used
               source_out    last source frame used (exclusive), > source_in
               generator     true when the media was produced, not imported
  out_path   absolute path the interchange file is written to

Frame counts are integers at the payload's rate. Gaps are implied by
non-contiguous timeline positions; writers emit explicit gap elements.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field


class PayloadError(ValueError):
    pass


@dataclass
class SegmentSpec:
    name: str
    path: str
    timeline_in: int
    timeline_out: int
    source_in: int
    source_out: int
    generator: bool = False


@dataclass
class TimelineSpec:
    title: str
    fps_num: int
    fps_den: int
    width: int
    height: int
    out_path: str
    segments: list[SegmentSpec] = field(default_factory=list)

    @property
    def nominal_rate(self) -> int:
        return (self.fps_num + self.fps_den // 2) // self.fps_den

    def frames_to_seconds(self, frames: int) -> str:
        """A frame count as FCPXML rational seconds, e.g. "96/24s"."""
        if frames == 0:
            return "0s"
        return f"{frames * self.fps_den}/{self.fps_num}s"


def _require_int(value, name: str, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise PayloadError(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        raise PayloadError(f"{name} must be at least {minimum}")
    return value


def parse_timeline_payload(payload: dict) -> TimelineSpec:
    if not isinstance(payload, dict):
        raise PayloadError("payload must be an object")

    fps = payload.get("fps")
    if not isinstance(fps, dict):
        raise PayloadError("fps must be an object")
    num = _require_int(fps.get("num"), "fps.num", 1)
    den = _require_int(fps.get("den"), "fps.den", 1)

    out_path = payload.get("out_path")
    if not isinstance(out_path, str) or not os.path.isabs(out_path):
        raise PayloadError("out_path must be an absolute path")

    spec = TimelineSpec(
        title=str(payload.get("title") or "Untitled"),
        fps_num=num,
        fps_den=den,
        width=_require_int(payload.get("width", 1920), "width", 1),
        height=_require_int(payload.get("height", 1080), "height", 1),
        out_path=out_path,
    )

    segments = payload.get("segments", [])
    if not isinstance(segments, list):
        raise PayloadError("segments must be a list")
    cursor = 0
    for index, entry in enumerate(segments):
        if not isinstance(entry, dict):
            raise PayloadError(f"segments[{index}] must be an object")
        timeline_in = _require_int(entry.get("timeline_in"), "timeline_in", 0)
        timeline_out = _require_int(entry.get("timeline_out"), "timeline_out", 1)
        source_in = _require_int(entry.get("source_in", 0), "source_in", 0)
        source_out = _require_int(
            entry.get("source_out", timeline_out - timeline_in), "source_out", 1
        )
        if timeline_out <= timeline_in:
            raise PayloadError(f"segments[{index}] timeline span is empty")
        if timeline_in < cursor:
            raise PayloadError(f"segments[{index}] overlaps the previous shot")
        if source_out <= source_in:
            raise PayloadError(f"segments[{index}] source span is empty")
        path = entry.get("path")
        if not isinstance(path, str) or not path:
            raise PayloadError(f"segments[{index}] path must be set")
        cursor = timeline_out
        spec.segments.append(
            SegmentSpec(
                name=str(entry.get("name") or os.path.basename(path)),
                path=path,
                timeline_in=timeline_in,
                timeline_out=timeline_out,
                source_in=source_in,
                source_out=source_out,
                generator=bool(entry.get("generator", False)),
            )
        )
    return spec


def file_uri(path: str) -> str:
    from pathlib import Path

    return Path(path).absolute().as_uri()
