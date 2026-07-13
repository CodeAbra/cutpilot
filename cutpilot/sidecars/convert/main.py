"""CutPilot convert sidecar.

Serves timeline interchange writers (FCPXML, OpenTimelineIO), the ComfyUI
workflow mapper, and the DaVinci Resolve import bridge to the desktop app
over authenticated loopback HTTP. Configuration comes from the environment:

  CUTPILOT_IPC_TOKEN     required; bearer token every request must present
  CUTPILOT_CONVERT_PORT  fixed port (defaults to an ephemeral one)

On startup the chosen port is announced on stdout as "CUTPILOT_LISTENING <port>".
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from convert_sidecar.server import serve

if __name__ == "__main__":
    raise SystemExit(serve())
