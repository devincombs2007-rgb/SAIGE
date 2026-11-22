# 0.3 graphics handoff for Codex (state as of 2026-11-22)

Everything here is unmerged and was produced while Codex was unavailable. The reviewable units are the individual PRs; branch
`claude/integration-0.3-pbr` (PR #54, DO NOT MERGE) is their union plus one test commit, used to validate them together.
No file under `include/saige`, `src/`, `apps/` or the root CMake is touched. All GPU evidence is **software Vulkan** (Mesa
llvmpipe on Linux, lavapipe on Windows); none of it is physical-GPU verification (tracked in #9).

## PRs, heads and dependencies

| PR | Branch (head) | Content | Depends on | Hosted CI |
|---|---|---|---|---|
| #46 | `pbr-tangent-generation` (0f2398b) | tangent generator, importer integration | main | green |
| #47 | `pbr-brdf-reference` (416292e) | CPU glTF BRDF reference | main | green |
| #48 | `pbr-tonemap-pass` (03364d7) | tone-map pass, CPU reference | main | green |
| #49 | `pbr-material-architecture` (0fff7d4) | architecture doc with approved decisions | main | docs |
| #50 | `pbr-m1-material-data` (48c4128) | M1: five slots, per-slot samplers and color space | main | green |
| #51 | `pbr-m2-metallic-roughness` (a71d49b) | M2: metallic-roughness, `pbr.hlsli`, env BRDF table (draft) | #50, contains #47 | green |
| #52 | `pbr-m3-normal-mapping` (47ac109) | M3: normal mapping, 48-byte vertex (draft) | #51, contains #46 | green |
| #53 | `pbr-m4-hdr-output` (8005441) | M4: HDR target and tone-map pass (draft) | #51, contains #48 | green when last checked, re-check |
| #54 | `integration-0.3-pbr` (bb122c7) | union of the above plus `test_combined_pbr.cpp` | all | green (Linux, ASan, Windows MSVC Debug and Release, Windows Vulkan) |
| #55 | `pbr-env-brdf-asset` (9b2e416) | embedded versioned env BRDF table (draft) | #51 | green incl. MSVC |
| docs | `docs-0.3-handoff` | energy and table notes, MikkTSpace evaluation, alpha plan, this file | #49 | docs |

Suggested merge order (owner and Codex decide): #47, #46, #48, #49, #50, #51, #55, #52, #53. Each of the stacked PRs lists the commit to
review. Textual conflicts met when merging the stack (all resolved on #54): the test source list in
`engine/rhi_vulkan/CMakeLists.txt` (one line, every PR adds a file), a paragraph of `docs/rendering/gltf-subset.md` (M3 and
M4 both edit it), and `scene_renderer.cpp` (M2 and M4 each add a setting). After #50 and #51 land, retarget #52, #53 and #55 to
`main` and rebase; they apply cleanly in the order above.

Other graphics PRs from earlier work still open: #44 (Vulkan headers 1.4.363), #45 (capability probe, stacked on #44), #34 (draft combined 0.2
evidence, not to be merged). The 0.2 release candidate is untouched: branch `release-0.2.0-rc.1-prep` (875a771), notes
`docs/releases/0.2.0-rc.1.md`, nothing tagged or published.

## What the integrated renderer does

- A material has five texture slots (base color, metallic-roughness, normal, occlusion, emissive), each with its own sampler;
  base color and emissive are sRGB, the others linear; one GPU texture per (image, color space).
- `RendererSettings::model_shading` (default `Legacy` = the 0.2 look) and `RendererSettings::hdr` (default off) are the only
  switches. With both defaults every 0.2 behavior and pixel test is unchanged.
- `MetallicRoughness`: GGX, height-correlated Smith, Schlick as in the glTF specification (verified, see
  brdf-energy-and-lut.md), sun plus hemispherical ambient through a split-sum table, normal mapping with the glTF frame, mirrored
  instances and back faces handled.
- `hdr`: scene in RGBA16_FLOAT, tone-map pass (ACES-inspired default, exposure in stops) to the output.

## Verification matrix

| Check | Where | Result |
|---|---|---|
| Full CTest incl. 0.2 regression suite on the union | local Linux, Mesa lavapipe, validation + synchronization validation | 42 of 42 |
| Same on hosted Engine/Graphics CI for the union | Linux lavapipe, Linux GCC ASan/UBSan, Windows MSVC Debug and Release, Windows MSVC Vulkan lane | all green on #54 |
| Windows window-system lane (Win32 + Mesa lavapipe, demo artifact) on the union | `windows-software-vulkan.yml` dispatched on `claude/integration-0.3-pbr` | queued, result not yet read |
| Combined PBR + normal map + emissive + occlusion + HDR vs CPU reference, all five textures from PNGs, importer-generated tangents | `engine/render_sink/tests/test_combined_pbr.cpp` | pass |
| Color-space correctness | same test: one image used as sRGB base color and linear occlusion (5 uploads); mutations (occlusion as sRGB, emissive as linear) fail | pass, mutations caught |
| Resize across four sizes | same test, plus `test_hdr.cpp` (content compared with a fresh renderer) | pass |
| Failed staging (undecodable image, out-of-range slot, malformed tangents) | same test: registry live count and active model's pixels unchanged | pass |
| Resource lifetime: replace a staged model with frames in flight | same test and `test_hdr.cpp`, synchronization validation on | pass, 0 validation errors |
| Shader mutations (22 across M1 to M4) | each against its PR's tests | all caught |

Not covered, and honest limits: no physical GPU; the validation layer on these runners does not detect descriptor shader-read
hazards (see `test_bind_groups.cpp`); NaN branches are not observable on llvmpipe; Windows results are lavapipe, not a driver.

## Commands to reproduce locally

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON
    cmake --build build
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json SAIGE_REQUIRE_VALIDATION=1 ctest --test-dir build --output-on-failure
    cmake --build build --target saige_regenerate_shaders   # after editing HLSL; CTest checks the generated headers
    cmake --build build --target saige_regenerate_env_brdf  # after changing the BRDF terms (bump kEnvBrdfTableVersion)
    python3 tools/analysis/dielectric_energy.py             # independent energy check (numpy)

## Decisions and work waiting on others

1. **Core (issue #17):** temporal frame data (stable instance identity, previous rendered-frame transforms, camera history,
   explicit teleport and camera-cut flags). Requirements posted; no interface change made. Per-instance material override schema
   is Core's.
2. **Owner:** when to flip `model_shading` and `hdr` defaults (they change the look of every scene); whether to offer the
   energy-conserving coupling as an opt-in (brdf-energy-and-lut.md); approval of a new third-party dependency (MikkTSpace, zlib)
   after the comparison has been run; the blend-state RHI contract and sort policy (sorted-alpha-blending-plan.md).
3. **MikkTSpace comparison has not been run**: the harness is committed (`tools/analysis/mikktspace_compare/`) but compiling and
   running the downloaded reference was blocked in this session. Someone with permission should run it and record the numbers in
   `mikktspace-evaluation.md`.
4. **Not started by design:** M5 (image-based lighting) and M6 (cascaded sun shadows) need new RHI contracts (cube/array textures,
   depth sampling and comparison samplers, render-to-layer) and shared decisions; sorted alpha blending is planned only.
5. The scene-pass GPU timestamp does not include the tone-map pass; diagnostics JSON has no tone-map entry yet.

## Housekeeping candidates (not done, owner's call)

Redundant branches `claude/integration-0.2-base` and `claude/native-scene-integration`; draft #34 after its components have all
landed; the temporary integration PR #54 should be closed (not merged) once the individual PRs are merged.
