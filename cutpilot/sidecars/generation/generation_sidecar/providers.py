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
from .procedural import RenderCanceled, render_png
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


_PROVIDERS = {
    "local": ProceduralProvider(),
    "openai": OpenAiImagesProvider(),
}


def provider_for(model: ModelInfo):
    provider = _PROVIDERS.get(model.provider)
    if provider is None:
        raise RuntimeError(f"Unsupported provider: {model.provider}")
    return provider
