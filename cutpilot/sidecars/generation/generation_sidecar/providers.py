"""Provider adapters and the router that picks one per model.

Every adapter implements generate(request, on_progress, is_canceled) and
returns a GenerationResult; slow vendor work stays inside the adapter so the
job queue only ever sees progress callbacks and a final result or error.
"""

from __future__ import annotations

import base64
import http.client
import ipaddress
import json
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
    submit_path: str = "/{model}"
    submit_method: str = "POST"
    submit_headers: dict = field(default_factory=dict)
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
    job_id_path: tuple = ("id",)
    poll_url_path: tuple | None = ("polling_url",)
    poll_path: str = ""
    poll_method: str = "GET"
    poll_interval_s: float = 0.5
    poll_backoff_factor: float = 1.5
    poll_backoff_ceiling_s: float = 5.0
    request_timeout_s: int = 30
    job_deadline_s: float = 60.0
    status_path: tuple = ("status",)
    success_states: tuple[str, ...] = ()
    failure_states: tuple[str, ...] = ()
    progress_path: tuple | None = None
    error_msg_path: tuple | None = None
    result_kind: str = "image"
    result_ref_path: tuple = ("result", "sample")
    result_fetch: str = "url"
    cancel_url_path: tuple | None = None
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


def _download_capped(url: str, max_bytes: int, timeout: int) -> bytes:
    """Fetch a vendor-supplied image URL under strict bounds: https only (or
    http to an explicit loopback host, matching the in-process transport), a
    connection pinned to a validated address, every redirect hop re-validated
    against the same gate, a hard read cap, and a timeout. Bytes are returned
    raw and never interpreted."""
    current = url
    for _ in range(_MAX_REDIRECTS + 1):
        scheme, host, port, pinned_ip, path = _guard_url(current)
        connection = _open_pinned(scheme, host, pinned_ip, port, timeout)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            if response.status in _REDIRECT_STATUSES:
                location = response.headers.get("Location")
                response.read()
                if not location:
                    raise RuntimeError("Vendor redirect is missing a target")
                current = urljoin(current, location)
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


def _round_multiple(value: int, multiple: int) -> int:
    if multiple <= 0:
        return value
    rounded = int(round(value / multiple)) * multiple
    return max(MIN_ASYNC_DIMENSION, min(MAX_ASYNC_DIMENSION, rounded))


def _build_async_body(desc: AsyncJobDescriptor, request: GenerationRequest):
    width = _round_multiple(request.width, desc.size_multiple)
    height = _round_multiple(request.height, desc.size_multiple)
    body = {desc.prompt_key: request.prompt}
    if desc.size_mode == "wh_fields":
        body[desc.width_key] = width
        body[desc.height_key] = height
    elif desc.size_mode == "size_string":
        body["size"] = desc.size_format.format(w=width, h=height)
    body.update(desc.extra_body)
    return body, width, height


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


def _fire_cancel(desc: AsyncJobDescriptor, last_poll, headers, timeout) -> None:
    """Best-effort cancel: when the descriptor names a cancel URL and the last
    poll carried one, POST it and ignore any failure. A descriptor without a
    cancel URL cancels cooperatively (stop polling)."""
    if desc.cancel_url_path is None or last_poll is None:
        return
    cancel_url = _select_optional(last_poll, desc.cancel_url_path)
    if not cancel_url:
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
        desc = ASYNC_JOB_DESCRIPTORS[request.model.provider]
        key = keys.lookup_key(desc.provider)
        if not key:
            raise MissingKeyError(desc.provider)

        if is_canceled():
            raise JobCanceled()
        on_progress(0.02)

        body, width, height = _build_async_body(desc, request)
        submit_url = desc.base_url + desc.submit_path.format(model=_slug(request.model))
        auth = {desc.auth_header: desc.auth_template.format(key=key)}
        submit_resp = _vendor_json(
            submit_url,
            desc.submit_method,
            {**desc.submit_headers, **auth},
            body,
            desc.request_timeout_s,
        )

        job_id = _select_optional(submit_resp, desc.job_id_path)
        if job_id is None:
            raise RuntimeError(f"{desc.provider} returned an unexpected response shape")
        if desc.poll_url_path is not None:
            poll_url = _select_optional(submit_resp, desc.poll_url_path)
            if poll_url is None:
                raise RuntimeError(
                    f"{desc.provider} returned an unexpected response shape"
                )
        else:
            poll_url = desc.base_url + desc.poll_path.format(job_id=job_id)

        started = time.monotonic()
        interval = desc.poll_interval_s
        # Floor the synthetic progress at the value already streamed pre-submit
        # so the first in-loop emission can never regress below it.
        emitted = 0.02
        last_poll = None
        success_states = {s.casefold() for s in desc.success_states}
        failure_states = {s.casefold() for s in desc.failure_states}
        while True:
            if is_canceled():
                _fire_cancel(desc, last_poll, auth, desc.request_timeout_s)
                raise JobCanceled()
            if time.monotonic() - started > desc.job_deadline_s:
                raise RuntimeError(
                    f"{desc.provider} job did not complete within the time budget"
                )
            last_poll = _vendor_json(
                poll_url, desc.poll_method, auth, None, desc.request_timeout_s
            )
            status = str(_select_optional(last_poll, desc.status_path) or "")
            folded = status.casefold()
            if folded in success_states:
                break
            if folded in failure_states:
                detail = _select_optional(last_poll, desc.error_msg_path) or status
                raise RuntimeError(f"{desc.provider}: {detail}")
            # Any state we do not recognize keeps the loop polling; the whole-job
            # deadline is the backstop for a vendor that never settles.
            emitted = _synth_progress(desc, last_poll, started, emitted)
            on_progress(emitted)
            _sleep_cancelable(interval, is_canceled)
            interval = min(interval * desc.poll_backoff_factor, desc.poll_backoff_ceiling_s)

        if is_canceled():
            _fire_cancel(desc, last_poll, auth, desc.request_timeout_s)
            raise JobCanceled()

        ref = _select_optional(last_poll, desc.result_ref_path)
        if ref is None:
            raise RuntimeError(f"{desc.provider} returned an unexpected response shape")
        if desc.result_fetch == "url":
            data = _download_capped(ref, MAX_INPUT_FILE_BYTES, desc.request_timeout_s)
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
}

_sync_image = SyncImageProvider()
_async_job = AsyncJobProvider()
_PROVIDERS = {
    **{provider_id: _sync_image for provider_id in SYNC_IMAGE_DESCRIPTORS},
    **{provider_id: _async_job for provider_id in ASYNC_JOB_DESCRIPTORS},
}


def provider_for(model: ModelInfo):
    provider = _MODEL_PROVIDERS.get(model.id) or _PROVIDERS.get(model.provider)
    if provider is None:
        raise RuntimeError(f"Unsupported provider: {model.provider}")
    return provider
