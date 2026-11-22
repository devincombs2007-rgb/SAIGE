# MikkTSpace

SAIGE can generate tangents with the reference [MikkTSpace](https://github.com/mmikk/MikkTSpace) implementation by Morten S.
Mikkelsen, the algorithm the glTF specification names for assets that have a normal texture but no `TANGENT` attribute.

| | |
| --- | --- |
| Upstream revision | `mmikk/MikkTSpace` commit `3e895b49d05ea07e4c2133156cfa94369e19e409` (`master` when pinned, 2026-11-22) |
| Files | `mikktspace.c` SHA-256 `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288`; `mikktspace.h` SHA-256 `17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0` |
| License | zlib (notice below, also at the top of `mikktspace.h`). Unaltered: SAIGE uses the files as published. |
| Where | `engine/gfx_assets`: fetched by CMake at the pinned URLs and verified against both hashes, **not vendored**; built as the isolated C target `saige_mikktspace` (warnings off for that target only); wrapped by `src/tangents_mikktspace.cpp` (`generate_tangents_mikktspace`) |
| Option | `SAIGE_GFX_MIKKTSPACE` (default ON); OFF builds without it and without the C language. Offline builds set `FETCHCONTENT_SOURCE_DIR_SAIGE_MIKKTSPACE_C` and `..._H` to directories holding the verified files |

Status: available as a function and covered by tests; the importer's default is still the Lengyel-style generator
(`generate_tangents`). Switching the default, and then the fallback's role, is a separate reviewed change
(`docs/rendering/mikktspace-evaluation.md`).

## Review notes

- Maintenance: the algorithm is frozen on purpose (changing it would change every baked normal map); the repository is stable.
- Security: two C files, no I/O and no global state beyond a fixed sort seed; it allocates with `malloc` and checks the results, and it
  trusts its callbacks. The wrapper therefore validates everything first: array sizes consistent, indices in range, all positions,
  normals and UVs finite, at most 50 million triangles (the reference sizes are `int`), colors sized to the vertices. Malformed input
  returns `Invalid` and is never modified. It runs at asset-import time on data already validated by the glTF importer, never on network
  input. The tangent tests run under the ASan/UBSan CI lane.
- Behavior difference from the Lengyel-style generator: output is per triangle corner; the wrapper duplicates a vertex where its
  corners need different tangents (different handedness or more than 1 degree apart) and counts them in `split_vertices`.
- Updating: change the commit and both hashes in `engine/gfx_assets/CMakeLists.txt` and this page together; rerun
  `test_tangents_mikktspace.cpp` and the comparison harness (`tools/analysis/mikktspace_compare`).

## License notice (zlib)

```
Copyright (C) 2011 by Morten S. Mikkelsen

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```
