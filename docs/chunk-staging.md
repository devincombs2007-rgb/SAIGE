# Deterministic chunk staging

ChunkStream prepares adjacent scene projects on the runtime thread between frames. It processes one chunk per stage call in ascending coordinate order. A complete requested set becomes visible at a frame boundary. Unchanged coordinates and project and collision paths reuse active or already staged payloads. A superseding request releases staging for chunks it no longer needs. Cancellation or failed import discards pending work while preserving the active set.

At a simulation boundary, a host can check `ready_to_commit` and read prepared collision from `staged_collision`. If collision rejects the candidate, call `cancel` and keep the old active set. Run this sequence on the runtime thread with no intervening request or stage call. Once the complete candidate is ready, build
its full collision set and let `CapsuleCharacter::replace_static_geometry` validate and adopt it before calling `commit_frame`.
Do both operations before the next simulation tick so the controller and chunk residents see the same boundary.

The combined course/streaming test loads adjacent box floors from disk and exercises a missing collision file, an overlapping candidate,
a successful crossing, and unloading the floor behind the character. A streamed host must keep its reset point resident or select a new supported reset
point before evicting it. Vehicle entry and exit will also need a resident, supported destination and a clearance check.

The count limit applies separately to the active and requested sets. The asset budget covers retained prepared geometry and encoded image bytes across both sets, counting reused chunks once. Retain headroom to prepare replacements while the old set stays active. Importer scratch allocations, metadata, collision boxes, and GPU allocations are outside this budget. Each staged collision file is separately bounded by its size and box count. Project files retain their existing v1 schema and persisted asset IDs.

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

Coordinates are unique signed 32-bit integer pairs. Project paths and the optional `collision` paths are portable relative paths inside the manifest directory. Existing entries without a collision path remain valid.
The loader permits missing chunk projects so they can fail at staging time while the current scene stays active. The manifest
is bounded to 1 MiB and 4,096 sources. `select_chunks` selects a square neighborhood in grid coordinates and returns sources in
deterministic order. Hosts can use `chunk_containing` to map world positions to half-open cells, including negative coordinates. The host still controls the count and byte budgets through `ChunkStream`. The tests drive a two-chunk
crossing, eviction, revisit, and failed request.


## Chunk collision file version 1

The host supplies an optional collision file path in each chunk request, including paths read from the manifest. The streamer stages it alongside the scene before publication. Collision files are separate from the scene and manifest formats. Boxes use Z-up simulation coordinates in meters.

```json
{"schema_version":1,"boxes":[{"min":[0,-4,-1],"max":[8,4,0]}]}
```

The reader preserves box order and rejects unrecognized versions and fields, non-finite or out-of-range coordinates, and extents
that collapse after conversion to float. It accepts up to 4,096 boxes and 1 MiB per file. A host can stage an empty list by authoring
an empty array. An absent or invalid file fails the load so the host can keep its previous collision and residency.


The native streamed course requires a collision file for every entry. It uses 8 m square cells at `coord * 8` in XY and a reset
point at `(4, 4, 0)` inside cell `[0, 0]`. Course coordinates are bounded to `[-31, 30]` on each axis. Neighbor selection keeps a
one-cell radius plus the reset cell. Collision files share a simulation origin. GPU scene composition and asynchronous IO are
outside this course slice.
