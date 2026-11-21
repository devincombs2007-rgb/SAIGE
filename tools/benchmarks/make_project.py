"""Generate a reproducible scene tree for project extraction measurements."""

import argparse
import json
from pathlib import Path


def node_id(index):
    return f"00000000-0000-0001-0000-{index + 1:012x}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--nodes", type=int, default=8192)
    args = parser.parse_args()
    if not 1 <= args.nodes <= 16384:
        parser.error("--nodes must be between 1 and 16384")

    nodes = [
        {
            "id": node_id(index),
            "parent": node_id((index - 1) // 4) if index else None,
            "mesh": "00000000-0000-0000-0000-000000000000",
            "position": [float(index % 4), 1, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1],
        }
        for index in range(args.nodes)
    ]
    document = {
        "schema_version": 1,
        "assets": [],
        "nodes": nodes,
        "camera": {
            "position": [-7, 0, 2],
            "rotation": [0, 0, 0, 1],
            "fov_y_radians": 1.0471976,
            "near_plane": 0.1,
        },
    }
    args.output.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
