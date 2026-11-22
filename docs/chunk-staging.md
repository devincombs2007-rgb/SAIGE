# Deterministic chunk staging

ChunkStream prepares adjacent scene projects on the runtime thread between frames. It processes one chunk per stage call in ascending coordinate order. A complete requested set becomes visible at a frame boundary. Unchanged coordinates and project paths reuse the active payload. Cancellation, a superseding request, or failed import discards pending work while preserving the active set.

The count limit applies separately to the active and requested sets. The asset budget covers retained prepared geometry and encoded image bytes across both sets, counting reused chunks once. Retain headroom to prepare replacements while the old set stays active. Importer scratch allocations, metadata, and GPU allocations are outside this budget. Project files retain their existing v1 schema and persisted asset IDs.

This slice tests staging with two neighboring coordinates, eviction and revisit. Runtime scene composition, a versioned manifest, GPU staging and retirement, and asynchronous scheduling follow in later slices.
