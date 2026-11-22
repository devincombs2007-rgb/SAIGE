# Deterministic chunk staging

ChunkStream prepares adjacent scene projects on the runtime thread between frames. It processes one chunk per stage call in ascending coordinate order. A complete requested set becomes visible at a frame boundary. Unchanged coordinates and project paths reuse the active payload. Cancellation, a superseding request, or failed import discards pending work while preserving the active set.

The count limit applies separately to the active and requested sets. The asset budget covers retained prepared geometry and encoded image bytes across both sets, counting reused chunks once. Retain headroom to prepare replacements while the old set stays active. Importer scratch allocations, metadata, and GPU allocations are outside this budget. Project files retain their existing v1 schema and persisted asset IDs.

This slice tests staging with two neighboring coordinates, eviction and revisit. Runtime scene composition, a versioned manifest, GPU staging and retirement, and asynchronous scheduling follow in later slices.

## World manifest version 1

The manifest is a separate UTF-8 JSON file. It leaves existing scene project files and node/asset IDs intact.

```json
{
  "schema_version": 1,
  "chunks": [
    {"coord": [0, 0], "project": "west/saige.scene.json"},
    {"coord": [1, 0], "project": "east/saige.scene.json"}
  ]
}
```

Coordinates are unique signed 32-bit integer pairs. Project paths are portable relative paths inside the manifest directory.
The loader permits missing chunk projects so they can fail at staging time while the current scene stays active. The manifest
is bounded to 1 MiB and 4,096 sources. `select_chunks` selects a square neighborhood in grid coordinates and returns sources in
deterministic order. Hosts can use `chunk_containing` to map world positions to half-open cells, including negative coordinates. The host still controls the count and byte budgets through `ChunkStream`. The tests drive a two-chunk
crossing, eviction, revisit, and failed request.
