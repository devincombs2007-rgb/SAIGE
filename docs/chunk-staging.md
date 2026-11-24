# Deterministic chunk staging

ChunkStream prepares adjacent scene projects through synchronous staging or one background CPU job. It processes one chunk per stage call in ascending coordinate order. A complete requested set becomes visible at a frame boundary. Unchanged coordinates and project and collision paths reuse active or already staged payloads. A superseding request releases staging for chunks it no longer needs. Cancellation or failed import discards pending work while preserving the active set.

At a simulation boundary, a host can check `ready_to_commit` and read prepared collision from `staged_collision`. If collision rejects the candidate, call `cancel` and keep the old active set. Run this sequence on the runtime thread with no intervening request or stage call. Once the complete candidate is ready, build
its full collision set and let `CapsuleCharacter::replace_static_geometry` validate and adopt it before calling `commit_frame`.
Do both operations before the next simulation tick so the controller and chunk residents see the same boundary.

The combined course/streaming test loads adjacent box floors from disk and exercises a missing collision file, an overlapping candidate,
a successful crossing, and unloading the floor behind the character. A streamed host must keep its reset point resident or select a new supported reset
point before evicting it. Vehicle entry and exit will also need a resident, supported destination and a clearance check.

The count limit applies separately to the active and requested sets. The asset budget covers retained prepared geometry and encoded image bytes across both sets, counting reused chunks once. The desktop also shares the prepared CPU scene between chunks that name the same immutable
project path, counting that scene once. Collision is staged independently for every chunk and must validate before the
shared scene can publish at a new coordinate. Distinct project paths and scene edits across sessions remain independent. Retain headroom to prepare replacements while the old set stays active. Importer scratch allocations, metadata, collision boxes, and GPU allocations are outside this budget. Each staged collision file is separately bounded by its size and box count. Project files retain their existing v1 schema and persisted asset IDs.

Tests cover neighboring coordinates, eviction, revisits, failed staging and native scene composition.

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
one-cell radius plus the reset cell. Collision files share a simulation origin, and their box tops may reach at most Z=240 m. Imported scene transforms are chunk-local. On a transition the host stages one GPU model per asset ID and adopts the composed render
world with collision and residency. Resident render entities keep their identities. If two chunks use the same asset ID they
must name the same normalized project-relative source. Assets at those paths are treated as immutable for the session. The renderer reuses matching resident GPU models and uploads at most one new model per fixed update after startup. A partial candidate stays inactive, and actual resource destruction follows the renderer’s in-flight retirement. Uploads remain synchronous. The course reports successful uploads and their CPU wall time. CPU preparation can run in the background.


## Origin-relative ramp courses

Manifest version 2 adds an integer `origin` cell and a local XYZ `checkpoint` in that cell. The host subtracts the origin in
64-bit integer space before converting to meters. Neighboring cells can be addressed near either signed 32-bit limit. Their
collision and model coordinates are authored relative to each cell. Version 1 keeps its shared collision coordinates.
The checkpoint remains resident and must be supported by walkable collision. After the player or driven vehicle moves four
cells (32 m) from the simulation origin, the desktop can move the origin to the current cell. It does so only when the
requested neighborhood and pinned cells fit the supported local frame. It stages collision and render placement in the
new frame before committing at a fixed-step boundary. Durable chunk coordinates, resident entity handles and controller
velocities survive the shift. Reset returns to the retained checkpoint and recenters when its neighborhood is ready.
For a version 2 course, `--course-checkpoint FILE.json` lets C save the grounded position into a new checkpoint
cell and release the previous residency pin. The save stores signed grid coordinates and cell-local feet. The host proves
the pose can boot with collision from its home cell, then atomically replaces the sidecar before changing the live reset.
A failed save keeps the previous checkpoint. Use one sidecar per world. Invalid saves fail before checkpoint publication.
The current checkpoint still pins its cell until relocated. Saving is explicit and synchronous; it does not run during traversal.

Collision file version 2 adds a `triangles` array alongside `boxes`. Each triangle lists three XYZ vertices. The same capsule
handles box and triangle contact and the camera checks both. The ramp example places a render mesh and collision faces in
the same cell. It uses a distant grid origin and a non-default checkpoint.


The playable vehicle mode retains the checkpoint alongside the current walking or driving neighborhood. On foot, leaving
the parked car's cell stores its logical cell, position and heading and removes it from rendering and simulation. Its stable
marker returns only once a streaming candidate restores clear, level support. A failed load keeps the record and the active world.
The authored car reset remains available at the checkpoint in courses without a save file. If a save file is selected,
a version 2 checkpoint sidecar stores the parked cell, local XYZ and heading alongside the player checkpoint. The car
can be dormant during the save and is restored only when its cell becomes resident. R in this mode moves the player to
the saved checkpoint and keeps the car in its current logical location. The version 1 walking sidecar still loads.
A saved car whose collision is missing or no longer supports it blocks reattachment and leaves the active world intact. While seated, the capsule's inactive state
is kept at the checkpoint. Exit requires a clear sweep and supported ground in the active collision set. The absolute grid address
in diagnostics is computed with integer arithmetic relative to the active origin. An origin epoch increments after each published shift.

Do not spend multiple GPU uploads in one render iteration during fixed-step catch-up. The candidate remains staged until the next
submission opens an upload slot. Unchanged resident sets bypass restaging. Uploads are still synchronous and a single asset can
exceed the frame budget.


The desktop uses one background CPU import at a time after boot. The worker owns its source paths and CPU result only.
Cancellation or supersession discards stale results, and the runtime never waits for I/O during a fixed step. Publication
still requires the complete candidate and runs on the simulation thread. The initial checkpoint loads synchronously.
Disk completion timing may change the tick that opens a streaming boundary. The synchronous path remains available for
deterministic replay and focused tests. Shutdown joins any outstanding import.


Desktop shutdown diagnostics report accepted CPU preparations, fresh project imports, failures, discarded background
results, accepted preparation wall time, and retained CPU asset bytes. The phase includes boot and file import, and
excludes polling delays, GPU submission, and failed or discarded job time. The retained count excludes scratch and
GPU memory. Existing GPU staging wall time is reported separately.
