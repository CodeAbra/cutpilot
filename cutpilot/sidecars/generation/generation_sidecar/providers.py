"""Provider adapters and the router that picks one per model.

Every adapter implements generate(request, on_progress, is_canceled) and
returns a GenerationResult; slow vendor work stays inside the adapter so the
job queue only ever sees progress callbacks and a final result or error.
"""

from __future__ import annotations

import base64
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass

from . import keys
from .procedural import (
    RenderCanceled,
    blend_rows,
    decode_png,
    encode_png,
    render_png,
    upscale_rows,
)
from .registry import ModelInfo

# Pacing between procedural render bands: keeps a cancellation window open and
# approximates the latency profile of a hosted model.
PROCEDURAL_BAND_PAUSE_S = 0.02

OPENAI_IMAGES_URL = "https://api.openai.com/v1/images/generations"
OPENAI_TIMEOUT_S = 120


class MissingKeyError(Exception):
    def __init__(self, provider: str):
        super().__init__(f"No API key configured for {provider}")
        self.provider = provider


class JobCanceled(Exception):
    pass


@dataclass
class GenerationRequest:
    model: ModelInfo
    prompt: str
    width: int
    height: int
    seed: int
    out_path: str
    input_path: str = ""


@dataclass
class GenerationResult:
    path: str
    cost_usd: float
    width: int
    height: int


ProgressFn = Callable[[float], None]
CanceledFn = Callable[[], bool]


class ProceduralProvider:
    """Deterministic offline generation: real image, real progress, no network."""

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        def paced_progress(fraction: float) -> None:
            on_progress(fraction)
            time.sleep(PROCEDURAL_BAND_PAUSE_S)

        try:
            render_png(
                request.prompt,
                request.seed,
                request.width,
                request.height,
                request.out_path,
                on_progress=paced_progress,
                is_canceled=is_canceled,
            )
        except RenderCanceled as exc:
            raise JobCanceled() from exc
        return GenerationResult(
            path=request.out_path,
            cost_usd=request.model.price_usd,
            width=request.width,
            height=request.height,
        )


class OpenAiImagesProvider:
    """OpenAI Images API. The key is the user's own (env or keychain)."""

    # The API accepts a fixed set of sizes; the closest aspect match is used.
    SIZES = ((1024, 1024), (1536, 1024), (1024, 1536))

    def _pick_size(self, width: int, height: int) -> tuple[int, int]:
        aspect = width / max(1, height)
        return min(self.SIZES, key=lambda s: abs(s[0] / s[1] - aspect))

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        key = keys.lookup_key("openai")
        if not key:
            raise MissingKeyError("openai")

        width, height = self._pick_size(request.width, request.height)
        payload = json.dumps(
            {
                "model": "gpt-image-1",
                "prompt": request.prompt,
                "size": f"{width}x{height}",
                "n": 1,
            }
        ).encode()

        on_progress(0.05)
        http_request = urllib.request.Request(
            OPENAI_IMAGES_URL,
            data=payload,
            headers={
                "Authorization": f"Bearer {key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(http_request, timeout=OPENAI_TIMEOUT_S) as response:
                body = json.loads(response.read())
        except urllib.error.HTTPError as exc:
            detail = ""
            try:
                detail = json.loads(exc.read()).get("error", {}).get("message", "")
            except Exception:
                pass
            raise RuntimeError(f"OpenAI request failed ({exc.code}): {detail}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"OpenAI request failed: {exc.reason}") from exc

        if is_canceled():
            raise JobCanceled()

        on_progress(0.9)
        image_b64 = body["data"][0]["b64_json"]
        with open(request.out_path, "wb") as handle:
            handle.write(base64.b64decode(image_b64))
        return GenerationResult(
            path=request.out_path,
            cost_usd=request.model.price_usd,
            width=width,
            height=height,
        )


# A valid input PNG at the largest accepted dimensions still compresses well
# under this, so the raw read is bounded before the file ever reaches the
# decoder — a truly huge file on disk is refused without loading it whole.
MAX_INPUT_FILE_BYTES = 64 * 1024 * 1024


def _decode_input(request: GenerationRequest) -> tuple[int, int, list[bytes]]:
    try:
        with open(request.input_path, "rb") as handle:
            data = handle.read(MAX_INPUT_FILE_BYTES + 1)
    except OSError as exc:
        raise RuntimeError(f"Input image could not be read: {exc}") from exc
    if len(data) > MAX_INPUT_FILE_BYTES:
        raise RuntimeError("Input image is too large to process")
    try:
        return decode_png(data)
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc


class ProceduralUpscaleProvider:
    """Deterministic offline 2x upscale of the input image."""

    FACTOR = 2

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        width, height, rows = _decode_input(request)

        def paced_progress(fraction: float) -> None:
            on_progress(fraction)
            time.sleep(PROCEDURAL_BAND_PAUSE_S)

        try:
            scanlines = upscale_rows(
                rows,
                width,
                height,
                factor=self.FACTOR,
                on_progress=paced_progress,
                is_canceled=is_canceled,
            )
        except RenderCanceled as exc:
            raise JobCanceled() from exc

        out_width, out_height = width * self.FACTOR, height * self.FACTOR
        with open(request.out_path, "wb") as handle:
            handle.write(encode_png(scanlines, out_width, out_height))
        return GenerationResult(
            path=request.out_path,
            cost_usd=request.model.price_usd,
            width=out_width,
            height=out_height,
        )


class ProceduralEditProvider:
    """Deterministic offline edit: the prompt-seeded procedural layer blended
    over the input image at its own size."""

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        width, height, rows = _decode_input(request)

        def paced_progress(fraction: float) -> None:
            on_progress(fraction)
            time.sleep(PROCEDURAL_BAND_PAUSE_S)

        try:
            scanlines = blend_rows(
                rows,
                width,
                height,
                request.prompt,
                request.seed,
                on_progress=paced_progress,
                is_canceled=is_canceled,
            )
        except RenderCanceled as exc:
            raise JobCanceled() from exc

        with open(request.out_path, "wb") as handle:
            handle.write(encode_png(scanlines, width, height))
        return GenerationResult(
            path=request.out_path,
            cost_usd=request.model.price_usd,
            width=width,
            height=height,
        )


# Model-specific adapters win over the provider family, so several local
# models can share the keyless "local" provider while running different code.
_MODEL_PROVIDERS = {
    "local/procedural-v1": ProceduralProvider(),
    "local/procedural-upscale-v1": ProceduralUpscaleProvider(),
    "local/procedural-edit-v1": ProceduralEditProvider(),
}

_PROVIDERS = {
    "openai": OpenAiImagesProvider(),
}


def provider_for(model: ModelInfo):
    provider = _MODEL_PROVIDERS.get(model.id) or _PROVIDERS.get(model.provider)
    if provider is None:
        raise RuntimeError(f"Unsupported provider: {model.provider}")
    return provider
