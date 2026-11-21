"""Launch a version 1 SAIGE sample project and stream its JSON result."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def unique_fields(pairs):
    fields = {}
    for key, value in pairs:
        if key in fields:
            raise ValueError(f"duplicate project field: {key}")
        fields[key] = value
    return fields


def read_frame_budget(path):
    project = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_fields)
    if not isinstance(project, dict) or set(project) != {"schema_version", "sample", "frames"}:
        raise ValueError("expected an object with schema_version, sample and frames")
    if type(project["schema_version"]) is not int or project["schema_version"] != 1:
        raise ValueError("unsupported project schema_version (expected integer 1)")
    if project["sample"] != "moving-marker":
        raise ValueError("unknown sample (expected moving-marker)")
    frames = project["frames"]
    if type(frames) is not int or not 0 <= frames < 2**64:
        raise ValueError("frames must be an unsigned 64-bit integer")
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="path to saige.project.json")
    parser.add_argument("--engine", required=True, type=Path, help="path to saige_headless executable")
    args = parser.parse_args()
    try:
        frames = read_frame_budget(args.project)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    try:
        return subprocess.run([str(args.engine.resolve()), "--json", "--frames", str(frames)]).returncode
    except OSError as error:
        print(f"could not start SAIGE: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
