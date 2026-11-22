# MikkTSpace comparison harness

`compare.cpp` runs SAIGE's tangent generator (`gfx::generate_tangents`, PR #46) and reference MikkTSpace on the same synthetic
meshes (UV sphere with seam and poles, sphere with stretched UVs, sphere with a mirrored half, a cube with per-face UVs, a curved
heightfield with sheared, non-uniformly scaled UVs) and reports, per triangle corner, the angle between the two tangents and
whether the handedness agrees. glTF's V axis is flipped for MikkTSpace as the Khronos sample viewer does.

Status: run on 2026-11-22; results are in docs/rendering/mikktspace-evaluation.md. It needs the third-party reference sources
(`mikktspace.c`, `mikktspace.h`, zlib license, https://github.com/mmikk/MikkTSpace) and the PR #46 tangent generator sources.

Reference revision used: `master` as fetched on 2026-11-22,
`mikktspace.c` sha256 `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288`,
`mikktspace.h` sha256 `17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0`.
Verify the hashes of whatever you download before building.

To run, with the reference in its own empty directory (verify the hashes first), the harness and build output elsewhere, and
nothing from the reference inside the repository:

    gcc -O2 -c mikktspace.c -o mikk.o
    g++ -O2 -std=c++20 -I. -I<repo>/engine/gfx_assets/include -I<repo>/engine/render/include \
        <repo>/tools/analysis/mikktspace_compare/compare.cpp <repo>/engine/gfx_assets/src/tangents.cpp mikk.o -o compare
    ./compare
