"""Deterministic procedural image renderer.

Turns (prompt, seed, size) into a smooth layered-wave composition and encodes
it as a PNG using only the standard library. The same inputs always produce
byte-identical output, which makes the offline generation path reproducible
and testable. Rendering proceeds in horizontal bands so a long render can
report progress and honour cancellation between bands.
"""

from __future__ import annotations

import hashlib
import math
import struct
import zlib
from collections.abc import Callable

BAND_ROWS = 16


class RenderCanceled(Exception):
    """Raised inside render_png when the caller's cancel check turns true."""


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def _params(prompt: str, seed: int, width: int, height: int) -> dict:
    digest = hashlib.sha256(f"{prompt}|{seed}|{width}x{height}".encode()).digest()

    def byte(i: int) -> int:
        return digest[i % len(digest)]

    def unit(i: int) -> float:
        return byte(i) / 255.0

    # Two anchor colours far enough apart to read as a palette, plus wave
    # frequencies and phases, all derived from the digest.
    color_a = (40 + byte(0) % 120, 40 + byte(1) % 120, 60 + byte(2) % 140)
    color_b = (
        min(255, 130 + byte(3) % 120),
        min(255, 110 + byte(4) % 130),
        min(255, 120 + byte(5) % 130),
    )
    return {
        "color_a": color_a,
        "color_b": color_b,
        "fx": 0.8 + unit(6) * 2.2,
        "fy": 0.8 + unit(7) * 2.2,
        "fd": 1.0 + unit(8) * 2.5,
        "fr": 1.5 + unit(9) * 4.5,
        "px": unit(10) * math.tau,
        "py": unit(11) * math.tau,
        "pd": unit(12) * math.tau,
        "pr": unit(13) * math.tau,
        "cx": 0.25 + unit(14) * 0.5,
        "cy": 0.25 + unit(15) * 0.5,
        "channel_shift": 0.06 + unit(16) * 0.10,
    }


def render_rows(
    prompt: str,
    seed: int,
    width: int,
    height: int,
    on_progress: Callable[[float], None] | None = None,
    is_canceled: Callable[[], bool] | None = None,
) -> list[bytes]:
    """Render the image as one filter-prefixed scanline per row."""
    p = _params(prompt, seed, width, height)
    ar, ag, ab = p["color_a"]
    br, bg, bb = p["color_b"]
    inv_w = 1.0 / max(1, width - 1)
    inv_h = 1.0 / max(1, height - 1)

    # Row-invariant horizontal terms are computed once.
    wave_x = [math.sin(x * inv_w * p["fx"] * math.tau + p["px"]) for x in range(width)]
    dx2 = [(x * inv_w - p["cx"]) ** 2 for x in range(width)]
    shift = p["channel_shift"]

    rows: list[bytes] = []
    for y in range(height):
        if is_canceled and y % BAND_ROWS == 0 and is_canceled():
            raise RenderCanceled()
        ny = y * inv_h
        wave_y = math.sin(ny * p["fy"] * math.tau + p["py"])
        dy2 = (ny - p["cy"]) ** 2
        diag_base = ny * p["fd"] * math.tau + p["pd"]
        row = bytearray(1 + width * 3)
        offset = 1
        for x in range(width):
            radial = math.sin((dx2[x] + dy2) * p["fr"] * math.tau + p["pr"])
            diag = math.sin(x * inv_w * p["fd"] * math.tau + diag_base)
            t = (wave_x[x] + wave_y + diag + radial) * 0.125 + 0.5
            vignette = 1.0 - 0.55 * (dx2[x] + dy2)
            tr = min(1.0, max(0.0, t))
            tg = min(1.0, max(0.0, t + shift * wave_y))
            tb = min(1.0, max(0.0, t - shift * radial))
            row[offset] = int((ar + (br - ar) * tr) * vignette)
            row[offset + 1] = int((ag + (bg - ag) * tg) * vignette)
            row[offset + 2] = int((ab + (bb - ab) * tb) * vignette)
            offset += 3
        rows.append(bytes(row))
        if on_progress and y % BAND_ROWS == BAND_ROWS - 1:
            on_progress((y + 1) / height)
    return rows


def encode_png(rows: list[bytes], width: int, height: int) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    body = zlib.compress(b"".join(rows), 9)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", header)
        + _chunk(b"IDAT", body)
        + _chunk(b"IEND", b"")
    )


def render_png(
    prompt: str,
    seed: int,
    width: int,
    height: int,
    path: str,
    on_progress: Callable[[float], None] | None = None,
    is_canceled: Callable[[], bool] | None = None,
) -> None:
    rows = render_rows(prompt, seed, width, height, on_progress, is_canceled)
    data = encode_png(rows, width, height)
    with open(path, "wb") as handle:
        handle.write(data)
