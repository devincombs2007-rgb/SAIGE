# MikkTSpace evaluation

Owner decision 4 (0.3 approval): evaluate reference MikkTSpace for production-quality asset compatibility; the Lengyel-style
generator (`gfx::generate_tangents`, PR #46) is the interim solution.

**Status: comparison complete. The wrapper is under review in #58.** Owner approval (0.3 review): MikkTSpace is conditionally approved as a
production dependency, with the exact upstream source and hashes pinned, the zlib notice retained and compatibility and security
checked; the Lengyel generator stays as a fallback until MikkTSpace passes its own tests and review.

## Comparison results (2026-11-22)

Run in an isolated layout: the downloaded reference in its own empty directory, hashes verified against the pinned values below
before building, the harness and build output in separate directories, nothing from it in the repository. The SAIGE generator
was built from the original #46 implementation at `0f2398b`. Results describe that revision.
`tools/analysis/mikktspace_compare/compare.cpp` runs SAIGE's `gfx::generate_tangents` and reference MikkTSpace on the same
indexed meshes and compares tangents per triangle corner (glTF's V flipped for MikkTSpace as the sample viewer does).

| mesh | corners | median angle | p99 | max | handedness differences |
|---|---|---|---|---|---|
| UV sphere 64x32, seam and poles | 11904 | 0.028 deg | 0.082 deg | 0.084 deg | 0 |
| same, u stretched 4x | 11904 | 0.028 deg | 0.082 deg | 0.084 deg | 0 |
| lower half mirrored on its own (split) equator vertices, as an exporter writes it | 11904 | 0.028 deg | 0.082 deg | 0.084 deg | 0 |
| cube with per-face UVs (hard edges) | 36 | 0 | 0 | 0 | 0 |
| curved heightfield, shared vertices, sheared and non-uniformly scaled UVs | 9600 | 0.034 deg | 0.423 deg | 0.445 deg | 0 |
| lower half mirrored but sharing the equator vertices (UVs fold across that band) | 11904 | 0.034 deg | 179.6 deg | 180 deg | 128 (1.08% of corners) |

Reading: on well-formed input (seams and mirrored islands split into separate vertices, which glTF and every exporter
require) the two generators agree to under half a degree with no handedness differences, so the interim generator is adequate
there. They disagree, completely, only on the last mesh, where mirrored and unmirrored triangles share vertices and the UVs
fold: MikkTSpace gives each side its own tangent space by grouping triangles by orientation, while the vertex-based
accumulation blends them. That input is malformed for tangent purposes (the specification says triangles with inconsistent
handedness have an undefined tangent space), but it is exactly the case where a different generator would produce visibly
different shading, and it shows the structural difference.

Limits of this evidence: synthetic meshes only, no corpus of real exported assets and no baked normal maps (those would need
authoring-tool output, which this environment does not have); angle differences were not rendered. The result supports
"adopt for conformance and robustness" and does not show a large visual problem on well-formed meshes.

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

## Acceptance criteria and outcome

Criteria set before the run: adopt MikkTSpace as the default if either holds, keeping the option to turn it off:
1. Our generator disagrees materially on any realistic case: more than 1 percent of corners differ by over 5 degrees, or any
   handedness disagreement on a non-degenerate triangle. Then MikkTSpace is needed for compatibility.
2. Even if the two agree on the synthetic set, the specification says MikkTSpace, and the cost is small (a vendored C file and a
   vertex-splitting step), so adopt it for assets without tangents anyway.

Outcome: criterion 1 is met only on the malformed folded-UV mesh (1.08 percent of corners, 128 handedness differences); on
well-formed meshes the two agree to 0.45 degrees. Criterion 2 carries the decision, as predicted. The regression threshold for the
Lengyel fallback against MikkTSpace on well-formed meshes: no corner differs by more than 1 degree and no handedness differs.
**Done as PR #58: the pinned source is fetched by CMake (commit and hashes verified, not vendored) and `generate_tangents_mikktspace`
has its own tests; the importer default is unchanged until #58 is reviewed. Making it the default, with Lengyel as the fallback, is a
separate change.**

## Security review notes (source read as text)

`mikktspace.c` allocates with `malloc` (triangle lists, group buffers, tspace arrays) and checks for NULL on the allocations it
makes; it trusts the callbacks for index validity, so the wrapper must validate indices, finite values and non-degenerate counts
first (the importer already does) and must cap triangle counts so `3 * triangles` cannot overflow `int` (the reference uses
`int` sizes: reject meshes above roughly 700 million corners, in practice far lower via the importer's limits). It has no file or
network I/O and no global state beyond a fixed sort seed. Build it as an isolated C target; run the integration tests under the
ASan/UBSan lane.

## Pinned reference

The comparison ran on 2026-11-22 using the zlib-licensed [MikkTSpace reference](https://github.com/mmikk/MikkTSpace)
at commit `3e895b49d05ea07e4c2133156cfa94369e19e409`. PR #58 checks both SHA-256 hashes at configure time.

- `mikktspace.c` — `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288`
- `mikktspace.h` — `17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0`
