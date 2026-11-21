import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURE = Path(__file__).resolve().parents[1] / "examples" / "textured-scene"


def run(tool, project, *, ok):
    process = subprocess.run([tool, project], check=False, capture_output=True, text=True, timeout=15)
    if ok:
        if process.returncode:
            raise RuntimeError(process.stderr)
        result = json.loads(process.stdout)
        assert result["scene_nodes"] == 3
        assert result["unique_assets"] == result["mesh_primitives"] == result["encoded_images"] == 1
        assert result["encoded_image_bytes"] > 0
    else:
        assert process.returncode == 1 and not process.stdout and process.stderr


def main():
    tool = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary) / "fixture"
        shutil.copytree(FIXTURE, directory)
        project = directory / "saige.scene.json"
        asset = directory / "checker-box.gltf"
        run(tool, project, ok=True)
        asset.write_text("{invalid", encoding="utf-8")
        run(tool, project, ok=False)
        asset.unlink()
        run(tool, project, ok=False)
        shutil.copyfile(FIXTURE / asset.name, asset)
        run(tool, project, ok=True)


if __name__ == "__main__":
    main()
