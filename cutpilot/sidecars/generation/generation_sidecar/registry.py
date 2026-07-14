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
    # A model whose vendor contract is not yet confirmed against a live key.
    # The server keeps these out of the model picker and key surface until
    # they are confirmed, while they stay resolvable for a direct smoke test.
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
)


def list_models() -> tuple[ModelInfo, ...]:
    return MODELS


def model_by_id(model_id: str) -> ModelInfo | None:
    for model in MODELS:
        if model.id == model_id:
            return model
    return None
