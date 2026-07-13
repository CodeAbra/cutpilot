"""DaVinci Resolve import bridge.

Hands an interchange file (FCPXML, EDL, or OTIO) to a running Resolve
through its scripting API. Resolve ships that API as a Python module inside
its own installation; when the installation or a running instance is
missing, the bridge refuses with a structured reason instead of guessing.
"""

from __future__ import annotations

import os
import sys

_MODULE_DIRS = {
    "darwin": (
        "/Library/Application Support/Blackmagic Design/DaVinci Resolve"
        "/Developer/Scripting/Modules"
    ),
    "win32": os.path.expandvars(
        "%PROGRAMDATA%\\Blackmagic Design\\DaVinci Resolve\\Support"
        "\\Developer\\Scripting\\Modules"
    ),
    "linux": "/opt/resolve/Developer/Scripting/Modules",
}


def _module_dir() -> str:
    override = os.environ.get("CUTPILOT_RESOLVE_MODULES", "")
    if override:
        return override
    return _MODULE_DIRS.get(sys.platform, "")


def import_timeline(path: str) -> dict:
    if not os.path.isfile(path):
        return {"ok": False, "reason": "file_not_found", "detail": path}

    module_dir = _module_dir()
    if not module_dir or not os.path.isdir(module_dir):
        return {
            "ok": False,
            "reason": "resolve_unavailable",
            "detail": "DaVinci Resolve's scripting modules were not found; "
            "install Resolve or import the exported file manually.",
        }

    if module_dir not in sys.path:
        sys.path.insert(0, module_dir)
    try:
        import DaVinciResolveScript as dvr  # noqa: N813
    except ImportError as error:
        return {
            "ok": False,
            "reason": "resolve_unavailable",
            "detail": f"scripting module failed to load: {error}",
        }

    resolve = dvr.scriptapp("Resolve")
    if resolve is None:
        return {
            "ok": False,
            "reason": "resolve_not_running",
            "detail": "DaVinci Resolve is installed but not running; start "
            "it and retry.",
        }

    manager = resolve.GetProjectManager()
    project = manager.GetCurrentProject() or manager.CreateProject(
        "CutPilot Export"
    )
    if project is None:
        return {
            "ok": False,
            "reason": "no_project",
            "detail": "Resolve refused to open or create a project.",
        }

    timeline = project.GetMediaPool().ImportTimelineFromFile(path)
    if timeline is None:
        return {
            "ok": False,
            "reason": "import_rejected",
            "detail": "Resolve did not accept the timeline file.",
        }
    return {"ok": True, "timeline": timeline.GetName()}
