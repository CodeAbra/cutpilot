"""The model registry: every generation model the service can route to.

Adding a model here surfaces it in the desktop app's model picker with no UI
change; the entry names the provider adapter that fulfils it and the price the
cost readout shows.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ModelInfo:
    id: str
    label: str
    provider: str
    price_usd: float
    needs_key: bool
    # What the model consumes: a text prompt, an input image, or both. The
    # server refuses submissions missing a required piece, and the desktop app
    # reads the same flags to gate a run before it is ever submitted.
    needs_prompt: bool = True
    needs_input: bool = False
    # The vendor slug placed in the request body's "model" field. Empty means
    # derive it from the id's part after the provider prefix.
    model_slug: str = ""
    category: str = "image"
    output_kind: str = "image"
    # The async-job descriptor a model routes through when it differs from the
    # provider id. Empty routes by provider. A non-empty value disambiguates two
    # shapes that share one provider id and key (a synchronous image row and an
    # asynchronous video row under the same vendor account).
    descriptor: str = ""
    # A model whose vendor contract is not yet confirmed against a live key.
    # The server keeps these out of the model picker and key surface, and a
    # keyed vendor row is refused by the run route until an operator opt-in
    # opens the manual smoke gate that confirms it.
    unverified: bool = False


MODELS: tuple[ModelInfo, ...] = (
    ModelInfo(
        id="local/procedural-v1",
        label="Procedural (local)",
        provider="local",
        price_usd=0.002,
        needs_key=False,
    ),
    ModelInfo(
        id="local/procedural-edit-v1",
        label="Procedural Edit (local)",
        provider="local",
        price_usd=0.003,
        needs_key=False,
        needs_input=True,
    ),
    ModelInfo(
        id="local/procedural-upscale-v1",
        label="Procedural Upscale (local)",
        provider="local",
        price_usd=0.001,
        needs_key=False,
        needs_prompt=False,
        needs_input=True,
    ),
    # A keyless local video result, kept out of the picker and key surface, that
    # exercises the video Done path end to end through the real service and
    # client. Its bytes are not a playable clip; real playback is proven with an
    # encoded fixture. No vendor row is touched, so nothing un-quarantines.
    ModelInfo(
        id="local/procedural-video-v1",
        label="Procedural Video (local)",
        provider="local",
        price_usd=0.004,
        needs_key=False,
        needs_prompt=False,
        category="video",
        output_kind="video",
        unverified=True,
    ),
    ModelInfo(
        id="openai/gpt-image-1",
        label="GPT Image 1",
        provider="openai",
        price_usd=0.042,
        needs_key=True,
    ),
    ModelInfo(
        id="recraft/recraftv4_1",
        label="Recraft V4.1",
        provider="recraft",
        price_usd=0.04,
        needs_key=True,
        model_slug="recraftv4_1",
    ),
    ModelInfo(
        id="google/gemini-2.5-flash-image",
        label="Nano Banana",
        provider="google",
        price_usd=0.039,
        needs_key=True,
        model_slug="gemini-2.5-flash-image",
    ),
    # Slug, host region (BytePlus international vs Volcengine China), and whether
    # the endpoint returns base64 rather than a URL are all unconfirmed against a
    # live key; kept unverified until a real generation settles them.
    ModelInfo(
        id="bytedance/seedream-4-0",
        label="Seedream 4.0",
        provider="bytedance",
        price_usd=0.03,
        needs_key=True,
        model_slug="seedream-4-0",
        unverified=True,
    ),
    # The exact slug, the poll-status enumeration beyond Ready/Error/Failed, and
    # the width/height step constraint are unconfirmed against a live key; kept
    # unverified until a real generation settles them.
    ModelInfo(
        id="bfl/flux-2-pro-preview",
        label="Flux 2 Pro",
        provider="bfl",
        price_usd=0.05,
        needs_key=True,
        model_slug="flux-2-pro-preview",
        category="image",
        output_kind="image",
        unverified=True,
    ),
    # The video roster. Each row's slug, poll-status enumeration, and result
    # path are unconfirmed against a live key and kept unverified — out of the
    # picker — until a real generation settles them.
    ModelInfo(
        id="runway/gen4-turbo",
        label="Runway Gen-4 Turbo",
        provider="runway",
        price_usd=0.25,
        needs_key=True,
        needs_input=True,
        model_slug="gen4_turbo",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # Slug, the canonical host (Agents vs classic Dream Machine), and the
    # result path are all unconfirmed against a live key.
    ModelInfo(
        id="luma/ray-3",
        label="Luma Ray 3",
        provider="luma",
        price_usd=0.4,
        needs_key=True,
        model_slug="ray-3.2",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # The v2 submit shape, the nested status/result paths, and the slug are
    # unconfirmed against a live key; this row carries the highest such risk.
    ModelInfo(
        id="leonardo/motion-2",
        label="Leonardo Motion 2.0",
        provider="leonardo",
        price_usd=0.3,
        needs_key=True,
        model_slug="motion_2.0",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # Shares the ByteDance key with Seedream; the slug and status enumeration
    # are unconfirmed against a live key. Routed by descriptor so its async
    # video shape does not collide with the synchronous Seedream image row.
    ModelInfo(
        id="bytedance/seedance-1-pro",
        label="Seedance 1 Pro",
        provider="bytedance",
        descriptor="seedance-video",
        price_usd=0.3,
        needs_key=True,
        model_slug="seedance-1-0-pro-250528",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # The exact multipart fields and the 202/200 completion semantics are
    # unconfirmed against a live key.
    ModelInfo(
        id="stability/image-to-video",
        label="Stability Image-to-Video",
        provider="stability",
        price_usd=0.2,
        needs_key=True,
        needs_input=True,
        needs_prompt=False,
        model_slug="image-to-video",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # Aggregator rows: one key each reaches many hosted models. The
    # underlying-model id / slug, status enumeration, and result path of each is
    # unconfirmed against a live key; kept unverified — out of the picker — until
    # a real generation settles them.
    ModelInfo(
        id="fal/flux-schnell",
        label="Flux Schnell (Fal)",
        provider="fal",
        price_usd=0.003,
        needs_key=True,
        model_slug="fal-ai/flux/schnell",
        category="image",
        output_kind="image",
        unverified=True,
    ),
    # MiniMax reached through Fal as a clean submit->poll->fetch, with no direct
    # file-retrieve step.
    ModelInfo(
        id="fal/minimax-hailuo-02",
        label="MiniMax Hailuo 02 (Fal)",
        provider="fal",
        price_usd=0.28,
        needs_key=True,
        model_slug="fal-ai/minimax/hailuo-02/standard",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    ModelInfo(
        id="replicate/flux-schnell",
        label="Flux Schnell (Replicate)",
        provider="replicate",
        price_usd=0.003,
        needs_key=True,
        model_slug="black-forest-labs/flux-schnell",
        category="image",
        output_kind="image",
        unverified=True,
    ),
    ModelInfo(
        id="higgsfield/soul-standard",
        label="Higgsfield Soul (Standard)",
        provider="higgsfield",
        price_usd=0.05,
        needs_key=True,
        model_slug="higgsfield-ai/soul/standard",
        category="image",
        output_kind="image",
        unverified=True,
    ),
    # Kling signs each call with an access/secret pair. The slug, host, poll
    # status enumeration, and result path are unconfirmed against a live key;
    # kept unverified — out of the picker — until a real generation settles them.
    ModelInfo(
        id="kling/kling-v2-master",
        label="Kling 2.0 Master",
        provider="kling",
        price_usd=0.35,
        needs_key=True,
        model_slug="kling-v2-master",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    # Google Veo reuses the google account (the same key as the Nano Banana image
    # row) and routes by descriptor so its async video shape does not collide
    # with that synchronous image row. The slug, base host, operation-name
    # prefix, and parameter field names are unconfirmed against a live key; kept
    # unverified — out of the picker — until a real generation settles them.
    ModelInfo(
        id="google/veo-3-1",
        label="Veo 3.1",
        provider="google",
        descriptor="veo",
        price_usd=0.4,
        needs_key=True,
        model_slug="veo-3.1-generate-preview",
        category="video",
        output_kind="video",
        unverified=True,
    ),
    ModelInfo(
        id="google/veo-3-1-i2v",
        label="Veo 3.1 (Image to Video)",
        provider="google",
        descriptor="veo",
        price_usd=0.4,
        needs_key=True,
        needs_input=True,
        model_slug="veo-3.1-generate-preview",
        category="video",
        output_kind="video",
        unverified=True,
    ),
)


def list_models() -> tuple[ModelInfo, ...]:
    return MODELS


def model_by_id(model_id: str) -> ModelInfo | None:
    for model in MODELS:
        if model.id == model_id:
            return model
    return None
