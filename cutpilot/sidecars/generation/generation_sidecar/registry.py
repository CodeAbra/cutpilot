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


MODELS: tuple[ModelInfo, ...] = (
    ModelInfo(
        id="local/procedural-v1",
        label="Procedural (local)",
        provider="local",
        price_usd=0.002,
        needs_key=False,
    ),
    ModelInfo(
        id="openai/gpt-image-1",
        label="GPT Image 1",
        provider="openai",
        price_usd=0.042,
        needs_key=True,
    ),
)


def list_models() -> tuple[ModelInfo, ...]:
    return MODELS


def model_by_id(model_id: str) -> ModelInfo | None:
    for model in MODELS:
        if model.id == model_id:
            return model
    return None
