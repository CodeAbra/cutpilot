"""Deterministic procedural image renderer.

Turns (prompt, seed, size) into a smooth layered-wave composition and encodes
it as a PNG using only the standard library. The same inputs always produce
byte-identical output, which makes the offline generation path reproducible
and testable. Rendering proceeds in horizontal bands so a long render can
report progress and honour cancellation between bands.

The module also decodes PNG inputs (8-bit RGB/RGBA, non-interlaced) and
derives new images from them — a nearest-neighbour upscale and a blend of the
procedural layer over the input — so image-consuming models run offline with
the same determinism as plain generation.
"""

from __future__ import annotations

import hashlib
import math
import struct
import zlib
from collections.abc import Callable

BAND_ROWS = 16

# Decoded input images are bounded so a crafted PNG cannot exhaust memory:
# the largest side is capped, and decompression is limited to the exact size
# the header declares, so a small file can never expand without bound.
MAX_INPUT_DIMENSION = 8192


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


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(data: bytes) -> tuple[int, int, list[bytes]]:
    """Decode an 8-bit RGB or RGBA non-interlaced PNG into plain RGB rows
    (no filter prefix). Raises ValueError for anything else."""
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("Input is not a PNG image")

    width = height = 0
    channels = 0
    idat = bytearray()
    offset = 8
    while offset + 8 <= len(data):
        (length,) = struct.unpack(">I", data[offset : offset + 4])
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            if len(payload) != 13:
                raise ValueError("Corrupt PNG header")
            width, height, depth, color, _, _, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if depth != 8 or color not in (2, 6) or interlace != 0:
                raise ValueError(
                    "Unsupported PNG: only 8-bit RGB/RGBA non-interlaced "
                    "images can be used as inputs"
                )
            if not (1 <= width <= MAX_INPUT_DIMENSION
                    and 1 <= height <= MAX_INPUT_DIMENSION):
                raise ValueError(
                    "Input image is too large: each side must be at most "
                    f"{MAX_INPUT_DIMENSION} pixels"
                )
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    if width <= 0 or height <= 0 or channels == 0 or not idat:
        raise ValueError("Corrupt PNG image")

    stride = width * channels
    expected = (stride + 1) * height

    # The header fully determines how many bytes a valid image decompresses
    # to, so the output is bounded to that size (plus one, to catch a payload
    # that claims more). A decompression bomb hits the ceiling and is refused
    # having expanded no further than a real image of the same dimensions.
    decompressor = zlib.decompressobj()
    try:
        raw = decompressor.decompress(bytes(idat), expected + 1)
    except zlib.error as exc:
        raise ValueError("Corrupt PNG image data") from exc
    if len(raw) != expected:
        raise ValueError("Corrupt PNG image data")

    rows: list[bytes] = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        line = bytearray(raw[start + 1 : start + 1 + stride])
        if filter_type == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                above_left = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, previous[i], above_left)) & 0xFF
        elif filter_type != 0:
            raise ValueError("Corrupt PNG image data")
        previous = line
        if channels == 4:
            rgb = bytearray(width * 3)
            for x in range(width):
                rgb[x * 3 : x * 3 + 3] = line[x * 4 : x * 4 + 3]
            rows.append(bytes(rgb))
        else:
            rows.append(bytes(line))
    return width, height, rows


def upscale_rows(
    rows: list[bytes],
    width: int,
    height: int,
    factor: int = 2,
    on_progress: Callable[[float], None] | None = None,
    is_canceled: Callable[[], bool] | None = None,
) -> list[bytes]:
    """Nearest-neighbour upscale of plain RGB rows into filter-prefixed
    scanlines ready for encode_png."""
    out: list[bytes] = []
    for y in range(height):
        if is_canceled and y % BAND_ROWS == 0 and is_canceled():
            raise RenderCanceled()
        source = rows[y]
        line = bytearray(1 + width * factor * 3)
        offset = 1
        for x in range(width):
            pixel = source[x * 3 : x * 3 + 3]
            for _ in range(factor):
                line[offset : offset + 3] = pixel
                offset += 3
        scanline = bytes(line)
        out.extend([scanline] * factor)
        if on_progress and y % BAND_ROWS == BAND_ROWS - 1:
            on_progress((y + 1) / height)
    return out


def blend_rows(
    rows: list[bytes],
    width: int,
    height: int,
    prompt: str,
    seed: int,
    mix: float = 0.45,
    on_progress: Callable[[float], None] | None = None,
    is_canceled: Callable[[], bool] | None = None,
) -> list[bytes]:
    """Blend the prompt-seeded procedural layer over plain RGB rows, returning
    filter-prefixed scanlines ready for encode_png."""
    layer = render_rows(prompt, seed, width, height)
    keep = 1.0 - mix
    out: list[bytes] = []
    for y in range(height):
        if is_canceled and y % BAND_ROWS == 0 and is_canceled():
            raise RenderCanceled()
        source = rows[y]
        overlay = layer[y]
        line = bytearray(1 + width * 3)
        for i in range(width * 3):
            line[1 + i] = int(source[i] * keep + overlay[1 + i] * mix)
        out.append(bytes(line))
        if on_progress and y % BAND_ROWS == BAND_ROWS - 1:
            on_progress((y + 1) / height)
    return out
