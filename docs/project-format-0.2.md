# SAIGE 0.2 scene project format

A scene descriptor is a UTF-8 JSON document with schema_version 1. It contains assets, nodes and camera. This is a separate format from the original headless sample's project descriptor and CLI.

- Each asset has a canonical 128-bit id and a path rooted at the descriptor's directory. Zero is reserved and cannot appear in the asset table. Paths must be relative with forward slashes, contain no empty or dot segments, and resolve to regular files under the project root. Asset content validation belongs to the glTF importer. Resolving a file here does not make the containing directory safe against concurrent external edits.
- Each node has its own nonzero canonical id, an optional parent ID (null for root), a mesh ID, a three-number position, an XYZW quaternion, and a positive three-number scale. Zero mesh selects the built-in primitive. Parent IDs can refer forward within the table. A scene is validated before returning it, including missing mesh references, duplicate IDs, hierarchy cycles and transform limits.
- The camera has position, rotation, fov_y_radians and near_plane. Pose vectors and the projection must be finite, the vertical field of view is between zero and pi, and the near plane is positive.
- Files are bounded to 4 MiB, arrays to 16,384 entries, and parsing depth to 16. Duplicate JSON keys and unknown fields are rejected. Save stages the result next to the destination and renames it only after a successful close. Power-loss durability and editor autosave are future work.

Use saige_project_tool validate FILE or saige_project_tool save-as FILE OUTPUT. Save-as resolves assets against the destination directory and validates before replacing it. The [hierarchy example](../examples/hierarchy/saige.scene.json) reopens consistently and uses built-in meshes. The current tool handles project data and does not invoke the renderer.

The optional SAIGE_BUILD_PROJECTS CMake target uses nlohmann/json 3.12.0 (MIT), fetched from the official archive with SHA-256 verification. The parser is private to saige::project. Configure with -DSAIGE_BUILD_PROJECTS=OFF to build without this dependency. Offline builds can provide the verified archive's extracted source via FETCHCONTENT_SOURCE_DIR_SAIGE_JSON.

## First CPU baseline

Use a Release build and run from the repository root:

```sh
python3 tools/benchmarks/make_project.py /tmp/saige-benchmark-8192.json --nodes 8192
./build/saige_project_tool benchmark examples/hierarchy/saige.scene.json 250
./build/saige_project_tool benchmark /tmp/saige-benchmark-8192.json 250
```

On one shared Linux Xeon Platinum 8370C runner with GCC 14.2, the three-node scene loaded in 0.20 ms and resolved at p95 0.00033 ms. The 8,192-node tree loaded in 75.63 ms and resolved at p50 1.42 ms and p95 1.96 ms. Its world matrices occupy 524,288 bytes. Matrix payload is a scoped CPU allocation figure, excludes ID tables and parser memory, and is not peak process or GPU memory. The benchmark resolves all nodes per sample and does not present any frames. GPU timings, full frame latency, and device-resource memory remain separate measurement tasks. These are reference observations, not CI thresholds.
