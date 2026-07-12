"""CutPilot generation sidecar.

Serves generation jobs to the desktop app over authenticated loopback HTTP.
Configuration comes from the environment:

  CUTPILOT_IPC_TOKEN         required; bearer token every request must present
  CUTPILOT_GEN_DIR           directory generated media is written to
  CUTPILOT_GEN_PORT          fixed port (defaults to an ephemeral one)
  CUTPILOT_DISABLE_KEYCHAIN  set to 1 to resolve vendor keys from env vars only

On startup the chosen port is announced on stdout as "CUTPILOT_LISTENING <port>".
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from generation_sidecar.server import serve

if __name__ == "__main__":
    raise SystemExit(serve())
