"""Provider adapters and the router that picks one per model.

Every adapter implements generate(request, on_progress, is_canceled) and
returns a GenerationResult; slow vendor work stays inside the adapter so the
job queue only ever sees progress callbacks and a final result or error.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import http.client
import ipaddress
import json
import os
import socket
import ssl
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from urllib.parse import urljoin, urlsplit

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

# A valid input PNG at the largest accepted dimensions still compresses well
# under this, so the raw read is bounded before the file ever reaches the
# decoder — a truly huge file on disk is refused without loading it whole. It is
# also the default per-row result cap for image jobs; video rows widen it.
MAX_INPUT_FILE_BYTES = 64 * 1024 * 1024


class MissingKeyError(Exception):
    def __init__(self, provider: str):
        super().__init__(f"No API key configured for {provider}")
        self.provider = provider


class JobCanceled(Exception):
    pass


def _b64url(data: bytes) -> str:
    """base64url with the padding stripped, as JWT segments are encoded."""
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def kling_bearer_headers(secrets: dict[str, str]) -> dict[str, str]:
    """A short-lived HS256 bearer token minted from the access key (the issuer)
    and the secret key (the signing key). The token is signed here and rides the
    submit and poll requests only; neither raw secret ever leaves this function.
    The 30-minute lifetime exceeds a video job's deadline, so one mint covers a
    whole run, and the -5s not-before absorbs minor clock skew."""
    now = int(time.time())
    header = {"alg": "HS256", "typ": "JWT"}
    payload = {"iss": secrets["access_key"], "exp": now + 1800, "nbf": now - 5}
    segments = [
        _b64url(json.dumps(header, separators=(",", ":")).encode()),
        _b64url(json.dumps(payload, separators=(",", ":")).encode()),
    ]
    signing_input = ".".join(segments).encode("ascii")
    signature = hmac.new(
        secrets["secret_key"].encode(), signing_input, hashlib.sha256
    ).digest()
    segments.append(_b64url(signature))
    return {"Authorization": "Bearer " + ".".join(segments)}


def higgsfield_headers(secrets: dict[str, str]) -> dict[str, str]:
    """Two credentials joined into the vendor's key header; neither is logged or
    returned anywhere else."""
    return {
        "Authorization": f"Key {secrets['api_key']}:{secrets['api_secret']}"
    }


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


class ProceduralVideoProvider:
    """Deterministic offline video result: writes seeded bytes to the .mp4 path
    so the video Done path runs end to end. The bytes are not a decodable clip;
    real playback is proven separately with an encoded fixture."""

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        payload = bytearray()
        bands = 24
        for band in range(bands):
            if is_canceled():
                raise JobCanceled()
            payload.extend((request.seed + band).to_bytes(4, "little", signed=False))
            on_progress((band + 1) / bands)
            time.sleep(PROCEDURAL_BAND_PAUSE_S)
        with open(request.out_path, "wb") as handle:
            handle.write(bytes(payload))
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


@dataclass(frozen=True)
class AsyncJobDescriptor:
    """A vendor's submit -> poll -> fetch job endpoint expressed as data.

    One row per async provider drives the shared adapter: where and how to
    submit, how to read the job id and the polling URL back, how to poll and
    map a status to a terminal state, how to synthesize progress, and how to
    fetch the finished result. Adding an async provider is a new row, not new
    adapter code. Timings are fields so a still-image budget and a video budget
    differ by data alone.
    """

    provider: str
    base_url: str
    auth_header: str = "Authorization"
    auth_template: str = "Bearer {key}"
    # When set, the auth headers are built from the provider's secrets rather
    # than from auth_header/auth_template — a signed token or a joined
    # multi-secret header. Called as auth_builder(secrets) and returns the full
    # header map. None keeps the declarative single-secret template.
    auth_builder: Callable[[dict], dict] | None = None
    submit_path: str = "/{model}"
    submit_method: str = "POST"
    submit_headers: dict = field(default_factory=dict)
    # When set, the model slug is placed in the request body under this key
    # (video vendors post to a fixed path and name the model in the body). The
    # slug is per-model registry data, so it cannot live in the static per-row
    # extra_body. None keeps the slug on the submit path (its default place).
    model_body_key: str | None = None
    prompt_key: str = "prompt"
    size_mode: str = "wh_fields"
    width_key: str = "width"
    height_key: str = "height"
    size_format: str = "{w}x{h}"
    # 0 leaves sizes untouched; a positive value rounds width/height to the
    # nearest multiple and clamps to the accepted dimension range, so a
    # vendor's step constraint is row data rather than engine logic.
    size_multiple: int = 0
    extra_body: dict = field(default_factory=dict)
    # An escape hatch for a vendor whose body is not a flat field map (a nested
    # content array). When set it is called as build_body(desc, request) and
    # returns the full body dict plus the reported width and height, bypassing
    # the flat builder. Unset keeps the declarative flat body.
    build_body: Callable | None = None
    # An input image carried in the body: the field it goes under and how it is
    # encoded ("" or "data_uri" for the JSON path, a multipart file part when
    # body_encoding is "multipart"). Empty means the row takes no input image.
    input_body_key: str = ""
    input_encoding: str = ""
    # How the submit body is encoded on the wire: a JSON object, or a
    # multipart/form-data payload (a file part for the input image plus scalar
    # parts from extra_body). The submit response is JSON either way.
    body_encoding: str = "json"
    job_id_path: tuple = ("id",)
    poll_url_path: tuple | None = ("polling_url",)
    poll_path: str = ""
    poll_method: str = "GET"
    poll_interval_s: float = 0.5
    poll_backoff_factor: float = 1.5
    poll_backoff_ceiling_s: float = 5.0
    request_timeout_s: int = 30
    job_deadline_s: float = 60.0
    # How completion is signaled: a status field in each poll's JSON body, or
    # the poll's HTTP status code (a running code such as 202 keeps polling; 200
    # is terminal success and its body is the result when result_fetch="bytes").
    status_source: str = "json_field"
    status_path: tuple = ("status",)
    success_states: tuple[str, ...] = ()
    failure_states: tuple[str, ...] = ()
    # On the http_code path, HTTP status codes that mean "still working" and
    # should keep polling. Any 4xx/5xx outside this set is treated as a terminal
    # job failure so a real vendor error settles at once instead of waiting out
    # the whole deadline. Row data so a vendor with a non-202 running code fits.
    non_terminal_poll_codes: tuple[int, ...] = (202,)
    progress_path: tuple | None = None
    error_msg_path: tuple | None = None
    result_kind: str = "image"
    result_ref_path: tuple = ("result", "sample")
    result_fetch: str = "url"
    # A per-row bound on the fetched result so a valid clip larger than the
    # image cap is not refused as too large; still a hard bound, never unlimited.
    # Image rows keep the default; video rows widen it.
    result_max_bytes: int = MAX_INPUT_FILE_BYTES
    cancel_url_path: tuple | None = None
    # A selector into the submit response naming a separate URL where the result
    # lives (a queue vendor returns only a status in the poll body). None keeps
    # the result in the last poll body, as every direct row does.
    result_envelope_url_path: tuple | None = None
    # A selector on the terminal status body for a vendor error a "success"
    # terminal state can still carry, so a completed-with-error settles as an
    # error rather than a blind result read. None means the state alone is
    # authoritative.
    terminal_error_path: tuple | None = None
    # A selector into the submit response for a cancel URL (a queue vendor names
    # it there, not in the poll body). None keeps the poll-body cancel behavior.
    cancel_url_submit_path: tuple | None = None
    # Extra host suffixes, beyond the descriptor's own base host, the key may be
    # sent to on an authed fetch of a vendor-supplied URL. Empty trusts only the
    # base host, so the key is never sent to a vendor-controlled external host.
    authed_host_suffixes: tuple[str, ...] = ()
    # An aggregator hosts many models with different result shapes, so the
    # model's output kind selects the result reference. An empty map keeps the
    # single result_ref_path for every direct row.
    result_ref_by_kind: dict[str, tuple] = field(default_factory=dict)
    expected_duration_s: float = 4.0


ASYNC_JOB_DESCRIPTORS: dict[str, AsyncJobDescriptor] = {
    # The base host, the exact slug substituted into submit_path, the size
    # multiple, and the progress/details field names are provisional pending a
    # live-key confirmation; each is row data, so confirming one is a one-line
    # edit. Submit and poll both send the raw key in an x-key header (not
    # Bearer); the result URL is fetched with no auth header.
    "bfl": AsyncJobDescriptor(
        provider="bfl",
        base_url="https://api.bfl.ai/v1",
        auth_header="x-key",
        auth_template="{key}",
        submit_path="/{model}",
        prompt_key="prompt",
        size_mode="wh_fields",
        size_multiple=32,
        job_id_path=("id",),
        poll_url_path=("polling_url",),
        poll_interval_s=0.5,
        poll_backoff_factor=1.5,
        poll_backoff_ceiling_s=3.0,
        request_timeout_s=30,
        job_deadline_s=60.0,
        status_path=("status",),
        success_states=("Ready",),
        failure_states=(
            "Error",
            "Failed",
            "Request Moderated",
            "Content Moderated",
            "Task not found",
        ),
        progress_path=("progress",),
        error_msg_path=("details",),
        result_kind="image",
        result_ref_path=("result", "sample"),
        result_fetch="url",
        cancel_url_path=None,
        # The submit response names a regional polling host under the vendor's
        # own domain, so the key-carrying poll gate must trust that domain.
        authed_host_suffixes=("bfl.ai",),
        expected_duration_s=4.0,
    ),
}

# The dimension range a rounded async request size is clamped into, matching
# the server's accepted bounds, and the slice a cancelable sleep re-checks in.
MIN_ASYNC_DIMENSION = 64
MAX_ASYNC_DIMENSION = 2048
CANCEL_SLICE_S = 0.05


_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "localhost"})
_REDIRECT_STATUSES = frozenset({301, 302, 303, 307, 308})
_MAX_REDIRECTS = 5

# Plaintext http to a loopback host is refused for vendor-supplied URLs in
# production so a compromised vendor cannot reach a local service. Only the
# in-process test transport, which binds its stubs to 127.0.0.1 over http,
# flips this on.
_ALLOW_LOOPBACK_HTTP = False


def _flags_internal(address) -> bool:
    return (
        address.is_private
        or address.is_link_local
        or address.is_reserved
        or address.is_loopback
        or address.is_multicast
        or address.is_unspecified
    )


def _is_internal_address(address) -> bool:
    """True when an address — or the IPv4 it embeds through an IPv6 transition
    scheme — falls in a range a fetch must never reach on a vendor's behalf
    (cloud metadata at 169.254.169.254, RFC-1918 hosts). Mapped/6to4/Teredo
    forms are normalized so the block holds regardless of interpreter version."""
    if _flags_internal(address):
        return True
    for embedded in (
        getattr(address, "ipv4_mapped", None),
        getattr(address, "sixtofour", None),
    ):
        if embedded is not None and _flags_internal(embedded):
            return True
    teredo = getattr(address, "teredo", None)
    if teredo is not None and any(_flags_internal(part) for part in teredo):
        return True
    return False


def _resolve_pinned_address(host: str, port: int, allow_internal: bool) -> str:
    """Resolve the host once, refuse if any resolved address is internal (unless
    this is the loopback-http exception), and return one validated address for
    the connection to pin — so the transport never performs a second,
    unvalidated resolution between the check and the connect."""
    try:
        infos = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    except socket.gaierror as exc:
        raise RuntimeError("Refusing an image URL whose host cannot be resolved") from exc
    pinned = None
    for info in infos:
        raw = info[4][0].split("%", 1)[0]
        try:
            address = ipaddress.ip_address(raw)
        except ValueError as exc:
            raise RuntimeError(
                "Refusing an image URL with an unparseable address"
            ) from exc
        if not allow_internal and _is_internal_address(address):
            raise RuntimeError("Refusing an image URL that targets an internal host")
        if pinned is None:
            pinned = raw
    if pinned is None:
        raise RuntimeError("Refusing an image URL whose host cannot be resolved")
    return pinned


def _guard_url(url: str) -> tuple[str, str, int, str, str]:
    """Enforce the fetch policy for one URL and return the connection parts for
    a pinned, validated address: (scheme, host, port, pinned_ip, path). https is
    required except for an explicit loopback http host (the in-process
    transport); every resolved address of an https host must be external. Shared
    by the initial fetch and every redirect hop so there is one enforcement
    point."""
    if not isinstance(url, str):
        # A vendor field that should hold a URL but is a number, object, or
        # null settles as an unexpected shape rather than crashing on urlsplit.
        raise RuntimeError("Refusing a URL of an unexpected shape")
    parts = urlsplit(url)
    scheme = parts.scheme.lower()
    host = parts.hostname or ""
    if scheme == "http":
        if not (_ALLOW_LOOPBACK_HTTP and host in _LOOPBACK_HOSTS):
            raise RuntimeError("Refusing a plaintext image URL")
        port = parts.port or 80
        allow_internal = True
    elif scheme == "https":
        port = parts.port or 443
        allow_internal = False
    else:
        raise RuntimeError("Unsupported image URL scheme")
    pinned_ip = _resolve_pinned_address(host, port, allow_internal)
    path = parts.path or "/"
    if parts.query:
        path = f"{path}?{parts.query}"
    return scheme, host, port, pinned_ip, path


class _PinnedHTTPConnection(http.client.HTTPConnection):
    """An http connection that connects to a pre-validated address while keeping
    the original hostname for the Host header — the transport performs no
    resolution of its own."""

    def __init__(self, host: str, pinned_ip: str, port: int, timeout: int):
        super().__init__(host, port, timeout=timeout)
        self._pinned_ip = pinned_ip

    def connect(self):
        self.sock = socket.create_connection(
            (self._pinned_ip, self.port), self.timeout
        )


class _PinnedHTTPSConnection(http.client.HTTPSConnection):
    """As above for https: the socket targets the validated address, but the
    Host header and TLS SNI keep the original hostname so certificate
    verification still holds."""

    def __init__(self, host: str, pinned_ip: str, port: int, timeout: int):
        super().__init__(host, port, timeout=timeout, context=ssl.create_default_context())
        self._pinned_ip = pinned_ip

    def connect(self):
        sock = socket.create_connection((self._pinned_ip, self.port), self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=self.host)


def _open_pinned(scheme: str, host: str, pinned_ip: str, port: int, timeout: int):
    if scheme == "https":
        return _PinnedHTTPSConnection(host, pinned_ip, port, timeout)
    return _PinnedHTTPConnection(host, pinned_ip, port, timeout)


def _download_capped(
    url: str,
    max_bytes: int,
    timeout: int,
    auth_headers=None,
    auth_desc=None,
) -> bytes:
    """Fetch a vendor-supplied image URL under strict bounds: https only (or
    http to an explicit loopback host, matching the in-process transport), a
    connection pinned to a validated address, every redirect hop re-validated
    against the same gate, a hard read cap, and a timeout. Bytes are returned
    raw and never interpreted.

    A headerless fetch (auth_headers None) is the pre-signed-asset default and
    every existing caller uses it, byte for byte. When auth headers are supplied
    the fetch carries the user's key, so the first hop's host must pass the
    descriptor's authed-host gate before the key is sent, and the headers are
    dropped on ANY redirect — the key never follows a 302 to a signed-storage
    host."""
    current = url
    headers = dict(auth_headers) if auth_headers else {}
    if headers and not _authed_host_allowed(current, auth_desc):
        # Paired with the caller's pre-check: never send the key to a host
        # outside the descriptor's own base host or its declared suffixes.
        raise RuntimeError("Refusing to send the key to an untrusted result host")
    for _ in range(_MAX_REDIRECTS + 1):
        scheme, host, port, pinned_ip, path = _guard_url(current)
        connection = _open_pinned(scheme, host, pinned_ip, port, timeout)
        try:
            connection.request("GET", path, headers=headers)
            response = connection.getresponse()
            if response.status in _REDIRECT_STATUSES:
                location = response.headers.get("Location")
                response.read()
                if not location:
                    raise RuntimeError("Vendor redirect is missing a target")
                current = urljoin(current, location)
                # The redirect target may be a signed-storage host the key must
                # never reach, so drop the auth headers before the next hop.
                headers = {}
                continue
            if response.status != 200:
                raise RuntimeError(
                    f"Vendor image fetch failed ({response.status})"
                )
            data = response.read(max_bytes + 1)
        finally:
            connection.close()
        if len(data) > max_bytes:
            raise RuntimeError("Vendor image is too large")
        return data
    raise RuntimeError("Vendor image URL redirected too many times")


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
        try:
            for step in desc.result_ref:
                leaf = leaf[step]
        except (KeyError, IndexError, TypeError) as exc:
            raise RuntimeError(
                f"{desc.provider} returned an unexpected response shape"
            ) from exc
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


def _select_optional(obj, path):
    """Walk a path of keys/indices, returning None on any missing or wrongly
    typed step rather than raising — for optional response fields."""
    leaf = obj
    for step in path:
        try:
            leaf = leaf[step]
        except (KeyError, IndexError, TypeError):
            return None
    return leaf


def _authed_host_allowed(url, desc) -> bool:
    """Whether a vendor-supplied URL may carry the user's key: its host must
    equal the descriptor's own base host or end with a declared trusted suffix.
    This is stricter than _guard_url, which only blocks internal hosts and would
    still permit an arbitrary external host — so a vendor-controlled URL cannot
    exfiltrate the key to an off-host address."""
    if not isinstance(url, str):
        return False
    try:
        host = (urlsplit(url).hostname or "").lower()
        base_host = (urlsplit(desc.base_url).hostname or "").lower()
    except ValueError:
        return False
    if not host:
        return False
    if base_host and host == base_host:
        return True
    return any(
        host == suffix or host.endswith("." + suffix)
        for suffix in desc.authed_host_suffixes
    )


def _vendor_json(url, method, headers, body, timeout):
    """Send one request to a vendor job endpoint and return its parsed JSON.

    The URL is run through the same gate as an image fetch (https, or http only
    to an explicit loopback host, pinned to a validated address) before
    connecting, so a vendor-supplied poll URL cannot reach an internal host. A
    JSON body is sent only when present. The response is read under a hard cap;
    a non-2xx status raises a generic error carrying neither the key nor the
    URL."""
    scheme, host, port, pinned_ip, path = _guard_url(url)
    connection = _open_pinned(scheme, host, pinned_ip, port, timeout)
    try:
        send_headers = dict(headers)
        payload = None
        if body is not None:
            payload = json.dumps(body).encode()
            send_headers["Content-Type"] = "application/json"
        connection.request(method, path, body=payload, headers=send_headers)
        response = connection.getresponse()
        status = response.status
        raw = response.read(MAX_INPUT_FILE_BYTES + 1)
    finally:
        connection.close()
    if len(raw) > MAX_INPUT_FILE_BYTES:
        raise RuntimeError("Vendor response is too large")
    if not (200 <= status < 300):
        raise RuntimeError(f"vendor request failed ({status})")
    return json.loads(raw)


def _vendor_poll_raw(url, method, headers, timeout, max_bytes):
    """Poll a vendor whose completion is the HTTP status code, not a JSON field.

    Runs the same gate as the JSON path (https or loopback http, pinned to a
    validated address), returns (status_code, raw_bytes) without raising on the
    status and without parsing JSON, and refuses a body larger than max_bytes.
    Redirects are not followed on a poll."""
    scheme, host, port, pinned_ip, path = _guard_url(url)
    connection = _open_pinned(scheme, host, pinned_ip, port, timeout)
    try:
        connection.request(method, path, headers=dict(headers))
        response = connection.getresponse()
        status = response.status
        raw = response.read(max_bytes + 1)
    finally:
        connection.close()
    if len(raw) > max_bytes:
        raise RuntimeError("Vendor response is too large")
    return status, raw


def _vendor_submit_multipart(url, method, headers, body_bytes, content_type, timeout):
    """Submit a multipart/form-data body through the same gate as the JSON
    submit and return the parsed JSON submit response. A non-2xx raises a
    generic error carrying neither the key nor the URL."""
    scheme, host, port, pinned_ip, path = _guard_url(url)
    connection = _open_pinned(scheme, host, pinned_ip, port, timeout)
    try:
        send_headers = dict(headers)
        send_headers["Content-Type"] = content_type
        connection.request(method, path, body=body_bytes, headers=send_headers)
        response = connection.getresponse()
        status = response.status
        raw = response.read(MAX_INPUT_FILE_BYTES + 1)
    finally:
        connection.close()
    if len(raw) > MAX_INPUT_FILE_BYTES:
        raise RuntimeError("Vendor response is too large")
    if not (200 <= status < 300):
        raise RuntimeError(f"vendor request failed ({status})")
    return json.loads(raw)


def _round_multiple(value: int, multiple: int) -> int:
    if multiple <= 0:
        return value
    rounded = int(round(value / multiple)) * multiple
    return max(MIN_ASYNC_DIMENSION, min(MAX_ASYNC_DIMENSION, rounded))


def _read_input_capped(path: str) -> bytes:
    """Read an input file under the raw byte cap without decoding it: the file
    is sent to the vendor as-is, not rendered, so no PNG decode runs here."""
    try:
        with open(path, "rb") as handle:
            data = handle.read(MAX_INPUT_FILE_BYTES + 1)
    except OSError as exc:
        # Generic message: the raw OSError string carries the local path, which
        # would ride the job error/SSE. The traceback still reaches stderr.
        raise RuntimeError("Input image could not be read") from exc
    if len(data) > MAX_INPUT_FILE_BYTES:
        raise RuntimeError("Input image is too large to process")
    return data


def _encode_input_data_uri(path: str) -> str:
    return "data:image/png;base64," + base64.b64encode(
        _read_input_capped(path)
    ).decode()


def _build_async_body(desc: AsyncJobDescriptor, request: GenerationRequest):
    if desc.build_body is not None:
        return desc.build_body(desc, request)
    width = _round_multiple(request.width, desc.size_multiple)
    height = _round_multiple(request.height, desc.size_multiple)
    body = {desc.prompt_key: request.prompt}
    if desc.model_body_key is not None:
        body[desc.model_body_key] = _slug(request.model)
    if desc.size_mode == "wh_fields":
        body[desc.width_key] = width
        body[desc.height_key] = height
    elif desc.size_mode == "size_string":
        body["size"] = desc.size_format.format(w=width, h=height)
    if (
        desc.input_body_key
        and request.input_path
        and desc.input_encoding == "data_uri"
    ):
        body[desc.input_body_key] = _encode_input_data_uri(request.input_path)
    body.update(desc.extra_body)
    return body, width, height


def _build_multipart_body(desc: AsyncJobDescriptor, request: GenerationRequest):
    """Build a multipart/form-data submit body with the standard library: a
    file part for the input image under input_body_key and a scalar part per
    extra_body field. Returns (body_bytes, content_type, width, height) — a
    distinct arity from the flat JSON builder's (body, width, height)."""
    boundary = "cutpilot" + hashlib.sha256(os.urandom(32)).hexdigest()
    parts: list[bytes] = []
    for name, value in desc.extra_body.items():
        parts.append(
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                f"{value}\r\n"
            ).encode()
        )
    if desc.input_body_key and request.input_path:
        image = _read_input_capped(request.input_path)
        header = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{desc.input_body_key}"; '
            f'filename="input.png"\r\n'
            f"Content-Type: image/png\r\n\r\n"
        ).encode()
        parts.append(header + image + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    content_type = f"multipart/form-data; boundary={boundary}"
    return b"".join(parts), content_type, request.width, request.height


def _build_seedance_body(desc: AsyncJobDescriptor, request: GenerationRequest):
    """Build the nested content body: a text part, and — when the row takes an
    input image and one is supplied — an image part carrying the input as a
    data URI. The slug and extra fields ride the same body."""
    content = [{"type": "text", "text": request.prompt}]
    if desc.input_body_key and request.input_path:
        content.append(
            {
                "type": "image_url",
                "image_url": {"url": _encode_input_data_uri(request.input_path)},
            }
        )
    body = {"model": _slug(request.model), "content": content}
    body.update(desc.extra_body)
    return body, request.width, request.height


def _build_replicate_body(desc: AsyncJobDescriptor, request: GenerationRequest):
    """Replicate nests the model inputs under an "input" object, unlike the flat
    builder. The slug rides the submit path, not the body. The per-model input
    dimensions stay out of the body until a live key confirms them."""
    return (
        {"input": {desc.prompt_key: request.prompt, **desc.extra_body}},
        request.width,
        request.height,
    )


# Video budgets run in minutes, and a valid clip is larger than a still image;
# both are per-row bounds, never unlimited.
VIDEO_JOB_DEADLINE_S = 300.0
VIDEO_POLL_INTERVAL_S = 6.0
VIDEO_POLL_CEILING_S = 12.0
VIDEO_RESULT_MAX_BYTES = 256 * 1024 * 1024

# The direct video roster. Each row's base host, slug placement, status
# enumeration, and result path are provisional pending a live-key confirmation;
# each is row data, so confirming one is a one-line edit. Every backing model is
# unverified until then. Seedance shares the ByteDance provider id (and ARK key)
# with Seedream but routes through its own descriptor id.
ASYNC_JOB_DESCRIPTORS.update(
    {
        "runway": AsyncJobDescriptor(
            provider="runway",
            base_url="https://api.dev.runwayml.com",
            submit_path="/v1/image_to_video",
            submit_headers={"X-Runway-Version": "2024-11-06"},
            model_body_key="model",
            prompt_key="promptText",
            input_body_key="promptImage",
            input_encoding="data_uri",
            size_mode="none",
            extra_body={"ratio": "1280:720", "duration": 5},
            job_id_path=("id",),
            poll_url_path=None,
            poll_path="/v1/tasks/{job_id}",
            status_path=("status",),
            success_states=("SUCCEEDED",),
            failure_states=("FAILED", "CANCELED"),
            result_ref_path=("output", 0),
            result_fetch="url",
            result_kind="video",
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "luma": AsyncJobDescriptor(
            provider="luma",
            base_url="https://agents.lumalabs.ai/v1",
            submit_path="/generations",
            model_body_key="model",
            prompt_key="prompt",
            size_mode="none",
            job_id_path=("id",),
            poll_url_path=None,
            poll_path="/generations/{job_id}",
            status_path=("state",),
            success_states=("completed",),
            failure_states=("failed",),
            result_ref_path=("assets", "video"),
            result_fetch="url",
            result_kind="video",
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "leonardo": AsyncJobDescriptor(
            provider="leonardo",
            base_url="https://cloud.leonardo.ai/api/rest",
            submit_path="/v2/generations",
            model_body_key="model",
            prompt_key="prompt",
            size_mode="none",
            job_id_path=("sdGenerationJob", "generationId"),
            poll_url_path=None,
            poll_path="/v1/generations/{job_id}",
            status_path=("generations_by_pk", "status"),
            success_states=("COMPLETE",),
            failure_states=("FAILED",),
            result_ref_path=("generations_by_pk", "generated_images", 0, "url"),
            result_fetch="url",
            result_kind="video",
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "seedance-video": AsyncJobDescriptor(
            provider="bytedance",
            base_url="https://ark.ap-southeast.bytepluses.com/api/v3",
            submit_path="/contents/generations/tasks",
            size_mode="none",
            build_body=_build_seedance_body,
            input_body_key="image_url",
            input_encoding="data_uri",
            extra_body={},
            job_id_path=("id",),
            poll_url_path=None,
            poll_path="/contents/generations/tasks/{job_id}",
            status_path=("status",),
            success_states=("succeeded",),
            failure_states=("failed",),
            result_ref_path=("content", "video_url"),
            result_fetch="url",
            result_kind="video",
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "stability": AsyncJobDescriptor(
            provider="stability",
            base_url="https://api.stability.ai",
            submit_path="/v2beta/image-to-video",
            body_encoding="multipart",
            input_body_key="image",
            submit_headers={"accept": "video/*"},
            size_mode="none",
            extra_body={"cfg_scale": 1.8, "motion_bucket_id": 127},
            model_body_key=None,
            job_id_path=("id",),
            poll_url_path=None,
            poll_path="/v2beta/image-to-video/result/{job_id}",
            status_source="http_code",
            result_fetch="bytes",
            result_kind="video",
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        # Kling's auth is a per-call HS256 token signed from an access/secret
        # pair; the JWT algorithm and claims are fixed here. The base host, the
        # submit and poll slugs, the status enumeration, and the result path are
        # provisional pending a live-key confirmation — each is row data, so
        # confirming one is a one-line edit. The token lifetime (1800s) exceeds a
        # video job's deadline, so one mint covers the whole run.
        "kling": AsyncJobDescriptor(
            provider="kling",
            auth_builder=kling_bearer_headers,
            base_url="https://api-singapore.klingai.com",
            submit_path="/v1/videos/text2video",
            model_body_key="model",
            prompt_key="prompt",
            size_mode="none",
            job_id_path=("data", "task_id"),
            poll_url_path=None,
            poll_path="/v1/videos/text2video/{job_id}",
            status_path=("data", "task_status"),
            success_states=("succeed",),
            failure_states=("failed",),
            error_msg_path=("message",),
            result_ref_path=("data", "task_result", "videos", 0, "url"),
            result_fetch="url",
            result_kind="video",
            authed_host_suffixes=("klingai.com",),
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
    }
)


# The aggregators: one key each reaches many hosted models, so a single
# descriptor serves both an image and a video model, selecting the result path
# by output kind. Each row's slug placement, status enumeration, result paths,
# and envelope host are provisional pending a live-key confirmation; each is row
# data, so confirming one is a one-line edit. Every backing model stays
# unverified until then. Fal returns only a status in the poll body and names
# the result at a separate response_url fetched with the key, so its fetch is
# restricted to Fal's own hosts; Replicate and Higgsfield return the result in
# the poll body. The video budgets are reused since one descriptor serves both
# an image and a video model.
ASYNC_JOB_DESCRIPTORS.update(
    {
        "fal": AsyncJobDescriptor(
            provider="fal",
            base_url="https://queue.fal.run",
            auth_header="Authorization",
            auth_template="Key {key}",
            submit_path="/{model}",
            model_body_key=None,
            size_mode="none",
            job_id_path=("request_id",),
            poll_url_path=("status_url",),
            result_envelope_url_path=("response_url",),
            status_path=("status",),
            success_states=("COMPLETED",),
            # Fal's queue status set is IN_QUEUE / IN_PROGRESS / COMPLETED with
            # no failure status; a hard failure arrives as COMPLETED carrying an
            # error field, settled by terminal_error_path below. The whole-job
            # deadline is the only backstop for an undocumented non-terminal
            # state, to be confirmed against the live status vocabulary.
            failure_states=(),
            terminal_error_path=("error",),
            result_fetch="url",
            result_ref_by_kind={
                "image": ("images", 0, "url"),
                "video": ("video", "url"),
            },
            cancel_url_submit_path=("cancel_url",),
            authed_host_suffixes=("fal.run", "fal.ai"),
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "replicate": AsyncJobDescriptor(
            provider="replicate",
            base_url="https://api.replicate.com",
            auth_header="Authorization",
            auth_template="Bearer {key}",
            submit_path="/v1/models/{model}/predictions",
            build_body=_build_replicate_body,
            model_body_key=None,
            size_mode="none",
            job_id_path=("id",),
            poll_url_path=("urls", "get"),
            status_path=("status",),
            success_states=("succeeded",),
            failure_states=("failed", "canceled"),
            result_fetch="url",
            # Replicate's output is a scalar URL or a list per model; the index
            # is row data, confirmed at the live gate.
            result_ref_by_kind={"image": ("output", 0), "video": ("output",)},
            cancel_url_path=("urls", "cancel"),
            authed_host_suffixes=("replicate.com",),
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
        "higgsfield": AsyncJobDescriptor(
            provider="higgsfield",
            base_url="https://platform.higgsfield.ai",
            # Two credentials joined into the vendor's key header.
            auth_builder=higgsfield_headers,
            submit_path="/{model}",
            model_body_key=None,
            size_mode="none",
            job_id_path=("request_id",),
            poll_url_path=("status_url",),
            status_path=("status",),
            success_states=("completed",),
            failure_states=("failed",),
            result_fetch="url",
            result_ref_by_kind={
                "image": ("images", 0, "url"),
                "video": ("video", "url"),
            },
            authed_host_suffixes=("higgsfield.ai",),
            poll_interval_s=VIDEO_POLL_INTERVAL_S,
            poll_backoff_ceiling_s=VIDEO_POLL_CEILING_S,
            job_deadline_s=VIDEO_JOB_DEADLINE_S,
            result_max_bytes=VIDEO_RESULT_MAX_BYTES,
        ),
    }
)


def _synth_progress(desc: AsyncJobDescriptor, resp, started: float, floor: float) -> float:
    """A monotonic progress fraction capped below 1.0: the vendor's own number
    when it reports one, otherwise elapsed time over the expected duration. The
    floor keeps a dipping vendor value from regressing the stream, and 1.0 is
    never emitted before the terminal state."""
    value = None
    if desc.progress_path is not None:
        raw = _select_optional(resp, desc.progress_path)
        if isinstance(raw, (int, float)) and not isinstance(raw, bool):
            value = float(raw) if 0.0 <= raw <= 1.0 else float(raw) / 100.0
    if value is None:
        value = (time.monotonic() - started) / desc.expected_duration_s
    value = min(0.99, max(0.0, value))
    return max(floor, value)


def _sleep_cancelable(interval: float, is_canceled: CanceledFn) -> None:
    """Sleep for interval seconds in short slices, returning early the moment a
    cancellation is requested so a stop is honored within a slice rather than
    after a whole backoff interval."""
    remaining = interval
    while remaining > 0:
        if is_canceled():
            return
        time.sleep(min(CANCEL_SLICE_S, remaining))
        remaining -= CANCEL_SLICE_S


def _fire_cancel(
    desc: AsyncJobDescriptor, last_poll, submit_cancel_url, headers, timeout
) -> None:
    """Best-effort cancel: prefer a cancel URL echoed in the last poll body,
    else one named in the submit response. The POST carries the key, so it fires
    only when the chosen URL is the aggregator's own host (the same trust gate as
    the authed result fetch); otherwise the cancel degrades cooperatively (stop
    polling). Any failure is ignored."""
    cancel_url = None
    if desc.cancel_url_path is not None and last_poll is not None:
        cancel_url = _select_optional(last_poll, desc.cancel_url_path)
    if not cancel_url and submit_cancel_url:
        cancel_url = submit_cancel_url
    if not cancel_url:
        return
    if not _authed_host_allowed(cancel_url, desc):
        return
    try:
        _vendor_json(cancel_url, "POST", headers, None, timeout)
    except Exception:
        pass


class AsyncJobProvider:
    """One adapter for every submit -> poll -> fetch vendor, driven by a
    descriptor row picked by the model's provider. The user's key rides the
    submit and poll requests only (never the pre-signed result fetch) and never
    appears in a progress update, error, or result."""

    def generate(
        self,
        request: GenerationRequest,
        on_progress: ProgressFn,
        is_canceled: CanceledFn,
    ) -> GenerationResult:
        desc = ASYNC_JOB_DESCRIPTORS[request.model.descriptor or request.model.provider]
        secrets = keys.lookup_secrets(desc.provider)
        if secrets is None:
            raise MissingKeyError(desc.provider)

        if is_canceled():
            raise JobCanceled()
        on_progress(0.02)

        submit_url = desc.base_url + desc.submit_path.format(model=_slug(request.model))
        if desc.auth_builder is not None:
            auth = desc.auth_builder(secrets)
        else:
            # A single-secret provider resolves to {"key": value}, so this
            # reproduces the shipped auth_template.format(key=value) byte for byte.
            auth = {desc.auth_header: desc.auth_template.format(**secrets)}
        submit_headers = {**desc.submit_headers, **auth}
        if desc.body_encoding == "multipart":
            body_bytes, content_type, width, height = _build_multipart_body(
                desc, request
            )
            submit_resp = _vendor_submit_multipart(
                submit_url,
                desc.submit_method,
                submit_headers,
                body_bytes,
                content_type,
                desc.request_timeout_s,
            )
        else:
            body, width, height = _build_async_body(desc, request)
            submit_resp = _vendor_json(
                submit_url,
                desc.submit_method,
                submit_headers,
                body,
                desc.request_timeout_s,
            )

        job_id = _select_optional(submit_resp, desc.job_id_path)
        if job_id is None:
            raise RuntimeError(f"{desc.provider} returned an unexpected response shape")
        if desc.poll_url_path is not None:
            # The poll URL comes straight from the submit response and is polled
            # with the key on every iteration, so it carries the same trust as
            # the envelope fetch: refuse a URL off the aggregator's own host
            # before the key is ever sent.
            poll_url = _select_optional(submit_resp, desc.poll_url_path)
            if poll_url is None or not _authed_host_allowed(poll_url, desc):
                raise RuntimeError(
                    f"{desc.provider} returned an unexpected response shape"
                )
        else:
            # A poll URL built from the descriptor's own hardcoded base is
            # already trusted and needs no host gate.
            poll_url = desc.base_url + desc.poll_path.format(job_id=job_id)

        # A queue vendor names the result and the cancel URL in the submit
        # response, not the poll body; retain each when the row declares it.
        envelope_url = None
        if desc.result_envelope_url_path is not None:
            envelope_url = _select_optional(submit_resp, desc.result_envelope_url_path)
        submit_cancel_url = None
        if desc.cancel_url_submit_path is not None:
            submit_cancel_url = _select_optional(submit_resp, desc.cancel_url_submit_path)

        started = time.monotonic()
        # Floor the cadence so a zero or non-growing poll interval cannot
        # busy-poll the vendor until the deadline.
        interval = max(CANCEL_SLICE_S, desc.poll_interval_s)
        # Floor the synthetic progress at the value already streamed pre-submit
        # so the first in-loop emission can never regress below it.
        emitted = 0.02
        last_poll = None
        result_bytes = None
        success_states = {s.casefold() for s in desc.success_states}
        failure_states = {s.casefold() for s in desc.failure_states}
        while True:
            if is_canceled():
                _fire_cancel(desc, last_poll, submit_cancel_url, auth, desc.request_timeout_s)
                raise JobCanceled()
            if time.monotonic() - started > desc.job_deadline_s:
                raise RuntimeError(
                    f"{desc.provider} job did not complete within the time budget"
                )
            if desc.status_source == "http_code":
                status_code, raw = _vendor_poll_raw(
                    poll_url,
                    desc.poll_method,
                    auth,
                    desc.request_timeout_s,
                    desc.result_max_bytes,
                )
                if status_code == 200:
                    # A 200 is terminal; its body is the result for a bytes row.
                    result_bytes = raw
                    break
                if (
                    400 <= status_code < 600
                    and status_code not in desc.non_terminal_poll_codes
                ):
                    # A 4xx/5xx is a terminal vendor error; surface it now
                    # instead of polling to the deadline. Code only, no key.
                    raise RuntimeError(
                        f"{desc.provider} poll failed ({status_code})"
                    )
                # A running code (202) or other non-terminal code keeps polling;
                # the whole-job deadline is the backstop.
            elif desc.status_source == "done_bool":
                last_poll = _vendor_json(
                    poll_url, desc.poll_method, auth, None, desc.request_timeout_s
                )
                done = _select_optional(last_poll, desc.status_path)
                if done:
                    # A truthy boolean done is terminal. A completion can still
                    # carry a vendor error, so read it before a blind result
                    # fetch and settle as an error.
                    if desc.terminal_error_path is not None:
                        terminal_error = _select_optional(
                            last_poll, desc.terminal_error_path
                        )
                        if terminal_error:
                            raise RuntimeError(f"{desc.provider}: {terminal_error}")
                    break
                # A falsy or absent done keeps polling; the whole-job deadline is
                # the backstop.
            else:
                last_poll = _vendor_json(
                    poll_url, desc.poll_method, auth, None, desc.request_timeout_s
                )
                status = str(_select_optional(last_poll, desc.status_path) or "")
                folded = status.casefold()
                if folded in success_states:
                    # A terminal success state can still carry a vendor error
                    # (a queue vendor's completed-with-error), so read it before
                    # a blind result fetch and settle as an error.
                    if desc.terminal_error_path is not None:
                        terminal_error = _select_optional(
                            last_poll, desc.terminal_error_path
                        )
                        if terminal_error:
                            raise RuntimeError(f"{desc.provider}: {terminal_error}")
                    break
                if folded in failure_states:
                    detail = status
                    if desc.error_msg_path is not None:
                        detail = _select_optional(last_poll, desc.error_msg_path) or status
                    raise RuntimeError(f"{desc.provider}: {detail}")
                # Any state we do not recognize keeps the loop polling; the
                # whole-job deadline is the backstop for a vendor that never
                # settles.
            emitted = _synth_progress(desc, last_poll, started, emitted)
            on_progress(emitted)
            _sleep_cancelable(interval, is_canceled)
            interval = min(interval * desc.poll_backoff_factor, desc.poll_backoff_ceiling_s)

        if is_canceled():
            _fire_cancel(desc, last_poll, submit_cancel_url, auth, desc.request_timeout_s)
            raise JobCanceled()

        if desc.result_fetch == "bytes":
            # The result is the terminal poll's own body, already read under the
            # per-row cap; the poll carried the key, so there is no separate
            # pre-signed fetch.
            if result_bytes is None:
                raise RuntimeError(
                    f"{desc.provider} returned an unexpected response shape"
                )
            data = result_bytes
        else:
            # An aggregator hosts many models with different result shapes, so
            # the model's output kind selects the reference; an empty map keeps
            # the single result_ref_path for every direct row.
            result_ref = desc.result_ref_by_kind.get(
                request.model.output_kind, desc.result_ref_path
            )
            if envelope_url is not None:
                # The result lives at a separate URL named in the submit
                # response, and this fetch carries the key — so refuse a URL
                # off the aggregator's own host before the key is ever sent.
                if not _authed_host_allowed(envelope_url, desc):
                    raise RuntimeError(
                        f"{desc.provider} returned an unexpected response shape"
                    )
                source = _vendor_json(
                    envelope_url, "GET", auth, None, desc.request_timeout_s
                )
            else:
                source = last_poll
            ref = _select_optional(source, result_ref)
            if ref is None:
                raise RuntimeError(
                    f"{desc.provider} returned an unexpected response shape"
                )
            if desc.result_fetch == "url":
                data = _download_capped(
                    ref, desc.result_max_bytes, desc.request_timeout_s
                )
            elif desc.result_fetch == "authed_url":
                # The result bytes live at a vendor-supplied URL that must be
                # fetched with the key. Refuse a host outside the descriptor's
                # own base host or declared suffixes before the key is ever sent;
                # the first-hop check inside _download_capped is the paired
                # defense, and the key is dropped on any redirect there.
                if not _authed_host_allowed(ref, desc):
                    raise RuntimeError(
                        f"{desc.provider} returned an unexpected response shape"
                    )
                data = _download_capped(
                    ref,
                    desc.result_max_bytes,
                    desc.request_timeout_s,
                    auth_headers=auth,
                    auth_desc=desc,
                )
            elif desc.result_fetch == "inline_b64":
                data = base64.b64decode(ref)
            else:
                raise RuntimeError(
                    f"{desc.provider} uses an unsupported result fetch mode"
                )
        with open(request.out_path, "wb") as handle:
            handle.write(data)
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
    "local/procedural-video-v1": ProceduralVideoProvider(),
}

_sync_image = SyncImageProvider()
_async_job = AsyncJobProvider()


def provider_for(model: ModelInfo):
    """Pick the adapter for a model by shape, not by a flat provider->adapter
    map. A model-specific adapter wins first. A set descriptor routes to the
    async adapter, so one provider id can host both a synchronous image row and
    an asynchronous video row under the same key. Otherwise the provider id
    selects sync or async, with the async provider id as the back-compat
    fallback for rows that route by provider alone."""
    specific = _MODEL_PROVIDERS.get(model.id)
    if specific is not None:
        return specific
    if model.descriptor and model.descriptor in ASYNC_JOB_DESCRIPTORS:
        return _async_job
    if model.provider in SYNC_IMAGE_DESCRIPTORS:
        return _sync_image
    if model.provider in ASYNC_JOB_DESCRIPTORS:
        return _async_job
    raise RuntimeError(f"Unsupported provider: {model.provider}")
