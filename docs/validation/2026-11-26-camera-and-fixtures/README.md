# Director camera clearance and fixture visual regression (Showcase 04 tooling)

* **Engine commit.** The code these images were rendered from is the head of `claude/s04-quality` containing this file; the exact 40-character SHA is recorded in the issue #43 checkpoint that accompanies the push (a file cannot contain the hash of the commit that contains it).
* **Technology.** The Showcase 04 camera director (`showcase/04-world-alive/render/director.cpp`), the district validator's camera rules (`src/district_validate.cpp`) and the fixture path of the visual regression (`tools/make_fixtures.py`, `saige_alive --fixtures`, `tools/visual_regression.py --fixtures`).
* **Backend and conditions.** Software Vulkan: Mesa llvmpipe 256-bit, 1280 x 720 for the camera pairs, 960 x 540 for the scenery pair, shadow map 2048, no anti-aliasing beyond the renderer's FXAA default. Film settings: seed 3 and seed 2 worlds, the scout report written for each seed by `alive_scout <seed> 206`, film of 180 s after 25 s of pre-roll at 30 fps. Not a physical GPU.

## What the images show

| File | What it shows | How it was made |
| --- | --- | --- |
| `camera_s3_f3485_before_after.jpg` | Seed 3, frame 3485, shot "the wreck". Left: the film's clearance behaviour with the film's fixed accident-camera sides (rendered from the pre-change binary): the eye is inside a wall, the view is of arcade fittings and bright shop panels. Right: the current director: the camera is in the street with the wrecked car in view. | `saige_alive --film --seed 3 --scout-file s3.txt --frames 3485`; left with `SAIGE_ALIVE_NO_CANOPY_CLEAR=1` from the binary of this branch before the accident cameras were chosen by geometry (the left frames cannot be re-rendered from the final tree, whose shots differ), right from the final tree |
| `camera_s2_f560_before_after.jpg` | Seed 2, frame 560, shot "the street reacts". Left: film behaviour (eye under an awning, dark slabs in front of the lens). Right: current director, standing by a facade looking at the crashed car. | same, seed 2, frame 560 |
| `scenery_clean_vs_legacy.jpg` | Fixed camera (the film's camera of frame 1170). Left: current renderer. Right: the film's scenery behaviour (`SAIGE_ALIVE_LEGACY_SCENERY=1`): a raised slab covers the avenue and z-fights at its edges. This is the picture the `scenery_does_not_cover_the_road` case guards. | fixture assets, `--still`, with and without the switch |
| `fixtures_vs_full.jpg` | The same fixed camera with the generated fixture assets (left) and the licensed Poly Haven textures and props (right). Shows what the fixture path does NOT cover: texture detail, props (lamp posts, benches, bins), the car model and the residents. | `--fixtures` against the restored asset cache |

| `seed4_none_cut_2482_2483.jpg` | Seed 4, report without an accident, frames 2482 and 2483 of the shot "follow a resident": the largest forced cut of the 16 films (8.2 m). Left: the camera along a plaster wall with a tree. Right: after the cut, behind the resident's head, which fills a third of the frame, looking at a crosswalk. | `saige_alive --film --seed 4 --scout-file <no-accident report> --frames 2482,2483` |
| `seed4_real_turn_4530_4531.jpg` | Seed 4, scouted report, frames 4530 and 4531 of "at the scene": the largest view turn outside a cut (23.9 degrees). A near-overhead view of the crashed car, which changes size and angle between the frames. | same, seed 4 real |
| `seed3_the_wreck_contact_sheet.jpg` | Four frames of the clip below (0.5, 2, 3.5 and 5.5 s). | `ffmpeg -ss <t> -i clip.mp4 -frames:v 1` |
| `seed3_the_wreck_720p30.mp4` | 6 s, 1280 x 720, 30 fps, H.264 CRF 18, no audio: seed 3, the "the wreck" shot (frames 3318 to 3498) with the final director and the licensed assets. | `saige_alive --film --seed 3 --scout-file s3.txt --start-frame 3318 --end-frame 3498 --video clip.mp4` |

## Looked at, and by whom

I (Claude Code) opened all images and the four frames of the contact sheet; I did **not** play the clip (I cannot watch it), so nothing is claimed about its motion beyond the measured numbers below. The director and the owner have not yet inspected them. In particular, on `camera_s3_f3485` the right-hand image passes every camera rule and still has a plain wall filling the left half of the frame: the rules do not judge composition, only obstruction. That is a limitation, not a pass.

A first bounded camera version passed every rule and its frames showed the camera pressed against a wall from under a metre: the rule "a wall within 1.2 m straight ahead" was added because of what the frames showed, not because a number failed (register entry S4-038).

## Measured (not looked at)

* 16 films (8 seeds x the scouted report and a report without an accident), 5400 frames each, walked without rendering by `alive_camera_test --all` (about 12 minutes in a Release build): 0 frames fail any rule, 0 frames without a passing eye, 0 frames beyond the 5 m / 2.5 m displacement budget, 0 re-framed, at most 2 jumps over 1.5 m per film, the largest 5.6 m (seed 4, a follow shot of a resident).
* The film's clearance behaviour (only the push out of collision boxes; `SAIGE_ALIVE_NO_CANOPY_CLEAR=1`) fails the same test on the current shots (seed 3: 594 of 5400 frames obstructed); a 1 m budget fails it too (both are ctest mutants that must fail). Measured on the previous binary (`9b691ad`, fixed accident-camera sides): 279 frames with the eye inside a wall on seed 3, 23 on seed 2.
* Fixture visual regression: 6 of 6 intended mutants caught (interior walls, hidden faces, scenery, temporal flicker, camera clearance, a human summary read as a scout file).

## Known limitations

* The rules judge obstruction and framing of a subject, not composition. A wall filling half the frame passes.
* The eye jumps (1 to 2 times per film, up to 5.6 m) when its standing offset stops passing and the nearest passing one is far: a cut the shot did not ask for.
* Shots that follow a resident show different residents than the delivered film (the corrected world simulates differently).
* The fixture path covers geometry, depth, shadows, the scenery and the director; it does not cover the third-party textures' detail, normal and ARM maps, the Khronos car, props or residents. Fixture results are not evidence of full-asset fidelity.
* Software Vulkan only.

## Reproduce

```
cmake --build build --target saige_alive alive_scout alive_camera_test
alive_scout 3 206 > s3.txt                      # format 1, the file saige_alive itself would write
saige_alive --film --seed 3 --scout-file s3.txt --frames 3485 --width 1280 --height 720 --out out        # current director
SAIGE_ALIVE_NO_CANOPY_CLEAR=1 saige_alive ... --frames 3485 --out out_before    # the film's clearance only (the shots differ from the left image, see above)
alive_camera_test --all                                                                                # the 16 films
python3 -I showcase/04-world-alive/tools/visual_regression.py --app build/showcase/saige_alive --scout-tool build/showcase/alive_scout --fixtures --mutants
```
