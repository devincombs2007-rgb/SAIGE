"""CTest entry: runs the offline unit tests with the tools venv if there is one; exits 77 (skipped) when the Python dependencies are not installed."""
import os, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parent
venv = Path(os.environ.get("SAIGE_TOOLS_HOME", Path.home() / ".saige-tools")) / "venv" / "bin" / "python"
py = str(venv) if venv.exists() else sys.executable
probe = subprocess.run([py, "-c", "import numpy, soundfile, jsonschema, PIL, pyloudnorm"], capture_output=True)
if probe.returncode != 0:
    print("production tool dependencies are not installed (run tools/production/setup.sh): skipped")
    sys.exit(77)
sys.exit(subprocess.run([py, "-W", "ignore", "-m", "unittest", "discover", "-s", str(here), "-v"]).returncode)
