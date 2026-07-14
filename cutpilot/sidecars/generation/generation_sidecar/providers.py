"""Provider adapters and the router that picks one per model.

Every adapter implements generate(request, on_progress, is_canceled) and
returns a GenerationResult; slow vendor work stays inside the adapter so the
job queue only ever sees progress callbacks and a final result or error.
"""

from __future__ import annotations

import base64
import ipaddress
import json
import socket
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from urllib.parse import urlsplit

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


@dataclass(frozen=True)
class SyncImageDescriptor:
    """A vendor's synchronous image endpoint expressed as data.

    One row per provider drives the shared adapter: where to POST, how to
    authenticate, how sizes are chosen, what extra body fields to send, and
    how to read the image back out of the response.
    """

    provider: str
    base_url: str
    generate_path: str = "/images/generations"
    auth_header: str = "Authorization"
    auth_template: str = "Bearer {key}"
    size_mode: str = "passthrough"
    sizes: tuple[tuple[int, int], ...] = ()
    size_format: str = "{w}x{h}"
    extra_body: dict = field(default_factory=dict)
    result_ref: tuple = ()
    result_fetch: str = "inline_b64"
    timeout_s: int = 120


SYNC_IMAGE_DESCRIPTORS: dict[str, SyncImageDescriptor] = {
    "openai": SyncImageDescriptor(
        provider="openai",
        base_url="https://api.openai.com/v1",
        generate_path="/images/generations",
        auth_header="Authorization",
        auth_template="Bearer {key}",
        size_mode="fixed_set",
        sizes=((1024, 1024), (1536, 1024), (1024, 1536)),
        extra_body={"n": 1},
        result_ref=("data", 0, "b64_json"),
        result_fetch="inline_b64",
        timeout_s=120,
    ),
    "recraft": SyncImageDescriptor(
        provider="recraft",
        base_url="https://external.api.recraft.ai/v1",
        size_mode="passthrough",
        extra_body={"response_format": "b64_json", "n": 1},
        result_ref=("data", 0, "b64_json"),
        result_fetch="inline_b64",
    ),
    "google": SyncImageDescriptor(
        provider="google",
        base_url="https://generativelanguage.googleapis.com/v1beta/openai",
        size_mode="passthrough",
        extra_body={"response_format": "b64_json", "n": 1},
        result_ref=("data", 0, "b64_json"),
        result_fetch="inline_b64",
    ),
    # The base URL region and whether the endpoint returns base64 or a URL are
    # provisional until a live key confirms them; the url fetch mode is the
    # conservative default until then.
    "bytedance": SyncImageDescriptor(
        provider="bytedance",
        base_url="https://ark.ap-southeast.bytepluses.com/api/v3",
        size_mode="passthrough",
        extra_body={},
        result_ref=("data", 0, "url"),
        result_fetch="url",
    ),
}


_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "localhost"})


def _resolves_to_internal_address(host: str) -> bool:
    """True when a host resolves to a private, link-local, reserved, loopback,
    multicast, or unspecified address — the ranges a fetch must never reach on
    a vendor's behalf (cloud metadata at 169.254.169.254, RFC-1918 hosts)."""
    try:
        infos = socket.getaddrinfo(host, None)
    except socket.gaierror:
        return True
    for info in infos:
        raw = info[4][0].split("%", 1)[0]
        try:
            address = ipaddress.ip_address(raw)
        except ValueError:
            return True
        if (
            address.is_private
            or address.is_link_local
            or address.is_reserved
            or address.is_loopback
            or address.is_multicast
            or address.is_unspecified
        ):
            return True
    return False


def _download_capped(url: str, max_bytes: int, timeout: int) -> bytes:
    """Fetch a vendor-supplied image URL under strict bounds: https only (or
    http to an explicit loopback host, matching the in-process transport), no
    URL that resolves to an internal address, a hard read cap, and a timeout.
    Bytes are returned raw and never interpreted."""
    parts = urlsplit(url)
    scheme = parts.scheme.lower()
    host = parts.hostname or ""
    if scheme == "http":
        if host not in _LOOPBACK_HOSTS:
            raise RuntimeError("Refusing a non-loopback plaintext image URL")
    elif scheme == "https":
        if _resolves_to_internal_address(host):
            raise RuntimeError("Refusing an image URL that targets an internal host")
    else:
        raise RuntimeError("Unsupported image URL scheme")

    http_request = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(http_request, timeout=timeout) as response:
        data = response.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise RuntimeError("Vendor image is too large")
    return data


def _slug(model: ModelInfo) -> str:
    return model.model_slug or model.id.split("/", 1)[1]


def _resolve_size(
    desc: SyncImageDescriptor, width: int, height: int
) -> tuple[int, int]:
    if desc.size_mode == "passthrough":
        return width, height
    # fixed_set: the endpoint accepts only a fixed set; pick the closest aspect.
    aspect = width / max(1, height)
    return min(desc.sizes, key=lambda s: abs(s[0] / s[1] - aspect))


class SyncImageProvider:
    """One adapter for every synchronous image vendor, driven by a descriptor
    row picked by the model's provider. The key is the user's own (env or
    keychain) and never appears in a progress update, error, or result."""

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        desc = SYNC_IMAGE_DESCRIPTORS[request.model.provider]
        key = keys.lookup_key(desc.provider)
        if not key:
            raise MissingKeyError(desc.provider)

        width, height = _resolve_size(desc, request.width, request.height)
        body = {
            "model": _slug(request.model),
            "prompt": request.prompt,
            "size": desc.size_format.format(w=width, h=height),
            **desc.extra_body,
        }
        payload = json.dumps(body).encode()

        on_progress(0.05)
        http_request = urllib.request.Request(
            desc.base_url + desc.generate_path,
            data=payload,
            headers={
                desc.auth_header: desc.auth_template.format(key=key),
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                http_request, timeout=desc.timeout_s
            ) as response:
                parsed = json.loads(response.read())
        except urllib.error.HTTPError as exc:
            detail = ""
            try:
                detail = json.loads(exc.read()).get("error", {}).get("message", "")
            except Exception:
                pass
            finally:
                exc.close()
            raise RuntimeError(
                f"{desc.provider} request failed ({exc.code}): {detail}"
            ) from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(
                f"{desc.provider} request failed: {exc.reason}"
            ) from exc

        if is_canceled():
            raise JobCanceled()

        on_progress(0.9)
        leaf = parsed
        for step in desc.result_ref:
            leaf = leaf[step]
        if desc.result_fetch == "url":
            if is_canceled():
                raise JobCanceled()
            image_bytes = _download_capped(
                leaf, MAX_INPUT_FILE_BYTES, desc.timeout_s
            )
        else:
            image_bytes = base64.b64decode(leaf)
        with open(request.out_path, "wb") as handle:
            handle.write(image_bytes)
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

_sync_image = SyncImageProvider()
_PROVIDERS = {provider_id: _sync_image for provider_id in SYNC_IMAGE_DESCRIPTORS}


def provider_for(model: ModelInfo):
    provider = _MODEL_PROVIDERS.get(model.id) or _PROVIDERS.get(model.provider)
    if provider is None:
        raise RuntimeError(f"Unsupported provider: {model.provider}")
    return provider
