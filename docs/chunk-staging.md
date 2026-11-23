# Deterministic chunk staging

ChunkStream prepares adjacent scene projects on the runtime thread between frames. It processes one chunk per stage call in ascending coordinate order. A complete requested set becomes visible at a frame boundary. Unchanged coordinates and project paths reuse active or already staged payloads. A superseding request releases staging for chunks it no longer needs. Cancellation or failed import discards pending work while preserving the active set.

At a simulation boundary, a host can check `ready_to_commit` and prepare dependent collision from `staged_chunk`. If collision rejects the candidate, call `cancel` and keep the old active set. Run this sequence on the runtime thread with no intervening request or stage call.

The count limit applies separately to the active and requested sets. The asset budget covers retained prepared geometry and encoded image bytes across both sets, counting reused chunks once. Retain headroom to prepare replacements while the old set stays active. Importer scratch allocations, metadata, and GPU allocations are outside this budget. Project files retain their existing v1 schema and persisted asset IDs.

This slice tests staging with two neighboring coordinates, eviction and revisit. Runtime scene composition, a versioned manifest, GPU staging and retirement, and asynchronous scheduling follow in later slices.
