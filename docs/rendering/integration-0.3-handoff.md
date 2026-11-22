# 0.3 graphics handoff for Codex (state as of 2026-11-22)

Everything here is unmerged and was produced while Codex was unavailable. The reviewable units are the individual PRs; branch
`claude/integration-0.3-pbr` (PR #54, **DO NOT MERGE**, an integration test branch only) is their union plus one test commit, kept
as combined evidence. No file under `include/saige`, `src/`, `apps/` or the root CMake is touched. All GPU evidence is **software
Vulkan** (Mesa llvmpipe on Linux, lavapipe on Windows); none of it is physical-GPU verification (tracked in #9).

## Owner decisions recorded (0.3 reviews)

1. Approved architecture and M1 to M6; the seven decisions in `material-architecture.md` (ACES-inspired default tone map, sorted
   alpha blending, per-instance overrides preserved with the schema left to Core, MikkTSpace evaluated, independent per-slot
   samplers and color space, temporal requirements with Core, no premature ray tracing or upscaler infrastructure).
2. **MikkTSpace** conditionally approved as a production dependency: pin exact source and hashes, retain the zlib notice, verify
   compatibility and security; any scoped comparison runs in an isolated environment (run: see `mikktspace-evaluation.md`); keep the Lengyel
   generator as fallback until MikkTSpace passes its own tests and review.
3. **Energy-conserving BRDF** approved as an optional model in M5: glTF-faithful shading stays the default; the alternative must be
   explicitly selectable, independently tested and documented; no silent change of existing material behavior
   (`brdf-energy-and-lut.md`).
4. **RHI blend state** approved: modular, backend-independent, backward compatible; sorted source-over blending, view-space
   bounding-sphere-center depth as the initial sort key, two-pass rendering for double-sided transparent meshes, sorting limits
   documented, validation and pixel regression tests before renderer integration (#57 is that RHI step; plan in
   `sorted-alpha-blending-plan.md`).
5. The **precomputed versioned 64x64 BRDF table** stays (#55), provided accuracy and cross-platform reproducibility stay verified
   (`saige_env_brdf_table_up_to_date` in CTest, run on Linux, ASan and Windows MSVC).
6. All 0.3 implementation branches stay independently reviewable; **#54 is not to be merged**.

## PRs, heads, dependencies and hosted CI

| PR | Branch (head) | Content | Depends on | Hosted CI |
|---|---|---|---|---|
| #44 | `vulkan-headers-1.4.363` (80cba80) | Vulkan-Headers pin 1.4.363, version probe | main | passed Engine and Graphics (owner confirmed) |
| #45 | `vulkan-capability-probe` (f711f0a) | Vulkan 1.4 capability probe, negotiation rules (draft) | #44 | passed Engine and Graphics (owner confirmed); Windows run 44999570680 confirmed the core-1.4 path |
| #46 | `pbr-tangent-generation` (0f2398b) | tangent generator, importer integration | main | green |
| #47 | `pbr-brdf-reference` (416292e) | CPU glTF BRDF reference | main | green |
| #48 | `pbr-tonemap-pass` (03364d7) | tone-map pass, CPU reference | main | green |
| #49 | `pbr-material-architecture` (0fff7d4) | architecture doc with approved decisions | main | docs |
| #50 | `pbr-m1-material-data` (48c4128) | M1: five slots, per-slot samplers and color space | main | green |
| #51 | `pbr-m2-metallic-roughness` (a71d49b) | M2: metallic-roughness, `pbr.hlsli`, env BRDF table (draft) | #50, contains #47 | green |
| #52 | `pbr-m3-normal-mapping` (47ac109) | M3: normal mapping, 48-byte vertex (draft) | #51, contains #46 | green |
| #53 | `pbr-m4-hdr-output` (8005441) | M4: HDR target and tone-map pass (draft) | #51, contains #48 | green |
| #54 | `integration-0.3-pbr` (bb122c7) | union of #46 to #53 plus `test_combined_pbr.cpp`. **Do not merge** | all of those | Engine and Graphics: all green (Linux lavapipe, Linux ASan/UBSan, Windows MSVC Debug and Release, Windows MSVC Vulkan). Dedicated Windows software-Vulkan workflow (`windows-software-vulkan.yml`, run 45018663394, job win32-lavapipe, all 14 steps): **passed** |
| #55 | `pbr-env-brdf-asset` (9b2e416) | embedded versioned 64x64 env BRDF table (draft) | #51 | green incl. MSVC reproducibility check |
| #56 | `docs-0.3-handoff` (this branch) | energy and table notes, MikkTSpace evaluation, alpha plan, this file, analysis tools (draft) | #49 | docs |
| #57 | `rhi-blend-state` (4a6ef4c) | RHI blend state, validation, pixel tests | main | check (pending when written) |
| #58 | `mikktspace-tangents` (041db3c) | `generate_tangents_mikktspace`, pinned reference fetched by hash, importer default unchanged (draft) | #46 | check (pending when written) |

Not part of 0.3 and untouched: #34 (draft combined 0.2 evidence PR, not to be merged), the 0.2 release candidate (branch
`release-0.2.0-rc.1-prep`, 875a771; notes `docs/releases/0.2.0-rc.1.md`; nothing tagged or published), and the validated 0.2 baseline on main.

## Recommended merge sequence (owner and Codex decide; each PR needs its own review)

1. Independent of everything else: #44 then #45 (Vulkan headers and capability probe); #57 (RHI blend state); #46 then #58 (tangents, then MikkTSpace); #47, #48; #49.
2. The material stack, in order: #50, then #51, then #55 (retarget #55 to main after #51; it is a follow-up to M2's table), then
   #52 and #53 (either order, both retargeted to main after #51).
3. #56 (docs) after #49, or squashed into #49's review.
4. Close #54 without merging once the individual PRs have landed.

Textual conflicts to expect (all resolved on #54): the test source list in `engine/rhi_vulkan/CMakeLists.txt` (every PR adds a
file, one line), a paragraph of `docs/rendering/gltf-subset.md` (M3 and M4 both edit it), `scene_renderer.cpp` (M2 and M4 each add
a setting), and `rhi_vulkan/CMakeLists.txt` and `rhi/CMakeLists.txt` again with #57 (it adds `test_blend.cpp`). After each merge,
retarget the next PR to main and rebase; a clean rebase is expected apart from those lines.

## Outstanding reviews and open items

| Item | Owner | Notes |
|---|---|---|
| Review of every PR above | Codex, owner approval for major merges | each stacked PR names the commit to review |
| Core temporal frame data (#17): instance identity, previous rendered-frame transforms, camera history, teleport and camera-cut flags | Core | requirements posted; no interface change made; per-instance override schema stays Core's |
| MikkTSpace: review and approve #58; then a separate change to make it the importer default with Lengyel as fallback | Codex review, owner approval of the dependency | comparison done (agrees within 0.45 degrees on well-formed meshes; differs on UV-fold meshes); source pinned to upstream commit 3e895b49 and two SHA-256 hashes |
| Renderer integration of blend state (sorted blended phase, double-sided two-pass, shader alpha, stats) | graphics, after #57 is reviewed | plan in `sorted-alpha-blending-plan.md` |
| M5 (image-based lighting plus the optional energy-conserving model) and M6 (cascaded sun shadows) | graphics | new RHI contracts (cube/array textures, depth sampling, render-to-layer); not started |
| Flip of the `model_shading` and `hdr` defaults | owner | they change the look of every scene; both default to the 0.2 behavior |
| Physical-GPU verification | owner / #9 | nothing here is physical |

## What the integrated renderer does

- A material has five texture slots (base color, metallic-roughness, normal, occlusion, emissive), each with its own sampler;
  base color and emissive are sRGB, the others linear; one GPU texture per (image, color space).
- `RendererSettings::model_shading` (default `Legacy` = the 0.2 look) and `RendererSettings::hdr` (default off) are the only
  switches. With both defaults every 0.2 behavior and pixel test is unchanged.
- `MetallicRoughness`: GGX, height-correlated Smith, Schlick as in the glTF specification (verified; the specification itself says
  its dielectric coupling is not energy conserving, see `brdf-energy-and-lut.md`), sun plus hemispherical ambient through a
  split-sum table, normal mapping with the glTF frame, mirrored instances and back faces handled.
- `hdr`: scene in RGBA16_FLOAT, tone-map pass (ACES-inspired default, exposure in stops) to the output.

## Verification matrix

| Check | Where | Result |
|---|---|---|
| Full CTest incl. 0.2 regression suite on the union | local Linux, Mesa lavapipe, validation + synchronization validation | 42 of 42 |
| Same on hosted Engine and Graphics workflows for the union (#54) | Linux lavapipe, Linux ASan/UBSan, Windows MSVC Debug and Release, Windows MSVC Vulkan lane | all green |
| Windows window-system lane (Win32 + Mesa lavapipe, demo artifact) on the union | `windows-software-vulkan.yml` dispatched on `claude/integration-0.3-pbr` | passed (run 45018663394, 14 of 14 steps); I read job and step conclusions, not the raw log |
| Combined PBR + normal map + emissive + occlusion + HDR vs CPU reference, five PNG textures, importer-generated tangents | `engine/render_sink/tests/test_combined_pbr.cpp` | pass |
| Color-space correctness | same test: one image used as sRGB base color and linear occlusion (5 uploads); mutations (occlusion as sRGB, emissive as linear) fail | pass, mutations caught |
| Resize across four sizes | same test, plus `test_hdr.cpp` (content compared with a fresh renderer) | pass |
| Failed staging (undecodable image, out-of-range slot, malformed tangents) | same test: registry live count and active model's pixels unchanged | pass |
| Resource lifetime: replace a staged model with frames in flight | same test and `test_hdr.cpp`, synchronization validation on | pass, 0 validation errors |
| Environment BRDF table reproducibility across toolchains | `saige_env_brdf_table_up_to_date` on #55 | pass on Linux, ASan and Windows MSVC |
| Blend state | #57 pixel tests (UNORM, sRGB, float; every factor and operation) | pass locally; hosted CI pending when written |
| Shader and renderer mutations (about 30 across M1 to M4, the table and blend) | each against its PR's tests | all caught |

Not covered, and honest limits: no physical GPU; the validation layer on these runners does not detect descriptor shader-read
hazards (see `test_bind_groups.cpp`); NaN branches are not observable on llvmpipe; Windows results are lavapipe, not a driver.

## Commands to reproduce locally

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON
    cmake --build build
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json SAIGE_REQUIRE_VALIDATION=1 ctest --test-dir build --output-on-failure
    cmake --build build --target saige_regenerate_shaders   # after editing HLSL; CTest checks the generated headers
    cmake --build build --target saige_regenerate_env_brdf  # after changing the BRDF terms (bump kEnvBrdfTableVersion)
    python3 tools/analysis/dielectric_energy.py             # independent energy check (numpy)

## Housekeeping candidates (not done, owner's call)

Redundant branches `claude/integration-0.2-base` and `claude/native-scene-integration`; draft #34 after its components have all
landed; PR #54 should be closed (not merged) once the individual PRs are merged.
