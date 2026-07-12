"""BYOK vendor key lookup.

A key is the user's own: it is read from the environment first, then from the
macOS keychain generic-password item (service "cutpilot", account = provider)
the desktop app writes. Keys are never logged and never echoed back over the
API — the service only ever reports whether one is present.
"""

from __future__ import annotations

import os
import subprocess
import sys

KEYCHAIN_SERVICE = "cutpilot"

ENV_VARS: dict[str, str] = {
    "openai": "OPENAI_API_KEY",
}


def _keychain_enabled() -> bool:
    if os.environ.get("CUTPILOT_DISABLE_KEYCHAIN") == "1":
        return False
    return sys.platform == "darwin"


def _read_keychain(provider: str) -> str | None:
    try:
        proc = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                provider,
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
    """The user's API key for a provider, or None when not configured."""
    env_var = ENV_VARS.get(provider)
    if env_var:
        value = os.environ.get(env_var, "").strip()
        if value:
            return value
    if _keychain_enabled():
        return _read_keychain(provider)
    return None


def has_key(provider: str) -> bool:
    return lookup_key(provider) is not None
