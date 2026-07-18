"""BYOK vendor key lookup.

A key is the user's own: it is read from the environment first, then from the
macOS keychain generic-password item (service "cutpilot") the desktop app
writes. A provider may need more than one secret (an access key plus a signing
secret, an API key plus an API secret); each secret is a named slot with its own
environment variable and keychain account. A single-secret provider is one slot
whose account is the provider id, so its lookup is unchanged. Secrets are never
logged and never echoed back over the API — the service only ever reports whether
a provider's secrets are all present.
"""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass

KEYCHAIN_SERVICE = "cutpilot"

ENV_VARS: dict[str, str] = {
    "openai": "OPENAI_API_KEY",
    "recraft": "RECRAFT_API_TOKEN",
    "google": "GEMINI_API_KEY",
    "bytedance": "ARK_API_KEY",
    "bfl": "BFL_API_KEY",
    # ByteDance Seedance shares the ARK key above; no separate row for it.
    "runway": "RUNWAYML_API_SECRET",
    "luma": "LUMA_API_KEY",
    "leonardo": "LEONARDO_API_KEY",
    "stability": "STABILITY_API_KEY",
    "ideogram": "IDEOGRAM_API_KEY",
    "elevenlabs": "ELEVENLABS_API_KEY",
    # Aggregators: one key each reaches many hosted models.
    "fal": "FAL_KEY",
    "replicate": "REPLICATE_API_TOKEN",
}


@dataclass(frozen=True)
class SecretSlot:
    """One named secret a provider needs: which environment variable supplies
    it, which keychain account stores it, and the label the desktop key surface
    shows beside its field. The account is the single source of truth both this
    service and the desktop app read and write, so the two never disagree."""

    name: str
    env_var: str
    account: str
    label: str


# Providers whose auth needs more than one secret. A single-secret provider is
# not listed here; slots_for synthesizes its one slot with account == provider
# id, so every shipped single-secret key resolves unchanged.
KEY_SPECS: dict[str, tuple[SecretSlot, ...]] = {
    "kling": (
        SecretSlot("access_key", "KLING_ACCESS_KEY", "kling.access_key", "Access Key"),
        SecretSlot("secret_key", "KLING_SECRET_KEY", "kling.secret_key", "Secret Key"),
    ),
    "higgsfield": (
        SecretSlot("api_key", "HIGGSFIELD_API_KEY", "higgsfield.api_key", "API Key"),
        SecretSlot(
            "api_secret",
            "HIGGSFIELD_API_SECRET",
            "higgsfield.api_secret",
            "API Secret",
        ),
    ),
}


def slots_for(provider: str) -> tuple[SecretSlot, ...]:
    """The ordered secret slots a provider needs. A multi-secret provider comes
    from KEY_SPECS; any other provider is one slot whose account is the provider
    id — the shipped single-secret scheme, byte-identical."""
    spec = KEY_SPECS.get(provider)
    if spec is not None:
        return spec
    return (SecretSlot("key", ENV_VARS.get(provider, ""), provider, "API Key"),)


def _keychain_enabled() -> bool:
    if os.environ.get("CUTPILOT_DISABLE_KEYCHAIN") == "1":
        return False
    return sys.platform == "darwin"


def _read_keychain(account: str) -> str | None:
    try:
        proc = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                account,
                "-w",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    value = proc.stdout.strip()
    return value or None


def lookup_key(provider: str) -> str | None:
    """The user's single API key for a provider, or None when not configured.
    The synchronous image path reads its key here; a single-secret provider's
    account is its provider id."""
    env_var = ENV_VARS.get(provider)
    if env_var:
        value = os.environ.get(env_var, "").strip()
        if value:
            return value
    if _keychain_enabled():
        return _read_keychain(provider)
    return None


def lookup_secrets(provider: str) -> dict[str, str] | None:
    """Every secret a provider needs, keyed by slot name, or None the moment any
    one is missing (all-or-nothing). Each slot resolves environment-first, then
    the keychain account. A single-secret provider resolves through the shipped
    single-key path, so its presence check is byte-identical."""
    if provider not in KEY_SPECS:
        value = lookup_key(provider)
        return {"key": value} if value else None
    secrets: dict[str, str] = {}
    for slot in KEY_SPECS[provider]:
        value = ""
        if slot.env_var:
            value = os.environ.get(slot.env_var, "").strip()
        if not value and _keychain_enabled():
            value = _read_keychain(slot.account) or ""
        if not value:
            return None
        secrets[slot.name] = value
    return secrets


def has_key(provider: str) -> bool:
    return lookup_secrets(provider) is not None
