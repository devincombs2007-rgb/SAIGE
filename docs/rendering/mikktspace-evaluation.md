# MikkTSpace evaluation

Owner decision 4 (0.3 approval): evaluate reference MikkTSpace for production-quality asset compatibility; the Lengyel-style
generator (`gfx::generate_tangents`, PR #46) is the interim solution.

**Status: partly done.** The specification text, the licence and the reference algorithm were examined and an integration is
designed. The numeric comparison with our generator is written (`tools/analysis/mikktspace_compare/`) but has **not been run**:
the session was not permitted to compile and execute the downloaded reference code. No comparison numbers are claimed here.

## What the glTF specification requires

Read from the Khronos specification text: "When tangents are not specified and the material has a normal texture, the tangent
vectors are calculated using default MikkTSpace algorithms with the vertex positions, normals (specified or generated as
mentioned above), and texture coordinates associated with the normal texture." The bitangent is `cross(normal, tangent.xyz) *
tangent.w`. Vertices of a triangle should have equal W; otherwise the triangle's tangent space is undefined. Compatibility with
glTF content authored against baked normal maps therefore means MikkTSpace, because artists bake normal maps in tangent spaces
computed by it (Blender, Substance, xNormal and the Khronos sample viewer all use it). A different generator produces visibly
different shading wherever the two tangent spaces differ, most at UV seams, mirrored UV islands, poles and strongly sheared UVs.

## Licence and dependency review

zlib licence (the notice is in the header of `mikktspace.h`; permission to use, alter and redistribute, altered versions must be
marked, notice retained). Compatible with SAIGE's other vendored code (stb, nlohmann json). Two C files, about 66 KB, no
dependencies beyond `<stdlib.h>`, `<string.h>`, `<math.h>`, `<assert.h>`. Revision examined as text (2026-11-22, `master`):
`mikktspace.c` sha256 `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288`, `mikktspace.h` sha256
`17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0`. The project has been stable for years (the algorithm is
frozen by design, since changing it would change every baked normal map); long-term maintenance risk is low, and the risk of
vendoring is the usual one for C with manual memory management (it uses `malloc`/`free`, no bounds checks on its inputs beyond
the callbacks'). Security review item: feed it only meshes that passed the importer's validation (indices in range, finite
positions, normals and UVs), which `generate_tangents` already requires.

## How it differs from the interim generator (from reading the algorithm)

- Output is **per triangle corner** (through callbacks), not per vertex. MikkTSpace merges corners that share position, normal
  and UV, builds groups of triangles with consistent orientation (it separates orientation-preserving from mirrored triangles
  that meet at a vertex) and averages within a group with angle weights, then orthonormalizes. The Lengyel-style generator
  accumulates over all triangles of an indexed vertex, so a vertex shared by a mirrored island and a normal island gets one
  blended tangent, where MikkTSpace gives each side its own. glTF requires seams to be split into separate vertices, which makes
  most real assets agree, and the difference concentrates where the file does not split a mirrored or sheared seam.
- Degenerate triangles and quads with one good triangle are handled explicitly (spaces copied from the good triangle).
- Using the output requires mapping corners back to vertices: where corners of one indexed vertex disagree, the vertex has to be
  split (new vertex, same position, normal and UV, different tangent). That changes vertex counts and needs the staging path to
  accept vertices the file did not contain.

## Integration design (if the comparison supports adopting it)

- Vendor `mikktspace.c/.h` under `third_party/mikktspace/` with the zlib notice and a `docs/third-party/mikktspace.md` page (as for
  stb_image: pinned hash, purpose, where it is compiled), built as an isolated C target with warnings relaxed for that target only
  (the MSVC `/W4 /WX` and `-Werror` lanes must not see it), behind a CMake option `SAIGE_GFX_MIKKTSPACE` (default ON once adopted).
- `gfx_assets`: a `generate_tangents_mikktspace(Primitive&)` that runs the callbacks over the indexed triangles, then rebuilds the
  vertex arrays, splitting vertices where corner tangents differ by more than an epsilon in direction or sign. `generate_missing_tangents`
  in the importer picks it; the Lengyel generator stays as a fallback when the option is off and as a regression oracle for
  tests on well-behaved meshes.
- glTF's V axis is flipped for MikkTSpace (`v' = 1 - v`), as the sample viewer does; the output sign maps to `tangent.w` directly.
- Importer warning text changes from "Lengyel-style, not MikkTSpace" to the generator in use.
- Tests: mesh fixtures with seams, poles, mirrored and sheared UVs; every output vertex tangent orthogonal to its normal to
  within 1e-4; handedness consistent per triangle (the spec's equal-W rule); determinism; the staged result rendered against
  an oracle that evaluates the normal map with the MikkTSpace frame.

## Acceptance criteria for the comparison (to apply when `compare.cpp` has been run)

Adopt MikkTSpace as the default if either of the following holds, and still keep the option to turn it off:
1. Our generator disagrees materially on any realistic case: more than 1 percent of corners differ by over 5 degrees, or any
   handedness disagreement on a non-degenerate triangle. Then MikkTSpace is needed for compatibility.
2. Even if the two agree on the synthetic set, the specification says MikkTSpace, and the cost is small (a vendored C file and a
   vertex-splitting step), so adopt it for assets without tangents anyway.

In practice the second criterion already carries the decision; the comparison will quantify the visual risk of staying on the
interim generator and set a regression threshold for the Lengyel fallback. **Recommendation: adopt reference MikkTSpace for
imported assets without tangents, as a separate reviewable PR (`gfx_assets` and `third_party` only), after the harness has been
run.** Owner approval is needed for the new third-party dependency (AGENTS.md: review new dependencies).

## Who needs to do what

Someone allowed to build the reference (the owner, Codex, or this session with explicit permission) runs the harness per its
README and records the numbers here. I have not vendored anything.
