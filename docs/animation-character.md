# Character animation (saige::animation)

Reusable humanoid animation on top of the stateless sampling in `engine/gfx_assets/animation.hpp`. Source: `engine/animation`. Runnable example: `saige_anim_character_demo`. Tools: `saige_anim_inspect`, `saige_anim_mocap_probe`.

## Ownership contract (record for Core; proposed on issue #43 because Core is away)

| Concern | Owner | How |
| --- | --- | --- |
| Where the character is: position, velocity, collision, slopes, steps, jumping | the physics controller (`CapsuleCharacter`) | authoritative; animation never moves it |
| Which pose the character has | `LocomotionController` | follows the physics: it is fed the character's **real** horizontal speed, grounded flag and vertical speed each tick (`locomotion_input`), never the input |
| Root motion | the clip is **not** allowed to move the character | locomotion cycles are played in place (`evaluate_in_place` removes the clip's travel); the clip's travel survives only as data (`ClipInfo::root_speed`, `ground_speed`, `stride_length`), which sets the playback rate. Clips that are meant to carry the character (a roll) are a later, explicit mode (backlog) |
| Foot contact with terrain | IK layer (`adapt_feet_to_ground`) | stateless, after the blend; the pelvis follows the ground; call only while grounded |
| Foot contact with walls | IK layer (`keep_feet_outside_obstacles`) | the capsule is narrower than a stride is long; a foot ahead of the capsule is pulled out of the wall |
| GPU skinning | Rendering (gfx_assets, render) | consumes the skin matrices of the final pose |

Why speed from the physics: a character pushing into a wall has speed 0, so its pose returns to the idle instead of running on the spot; a character on a slope or a slowing surface plays slower; a knock-back plays at the speed it really has. `saige_animation_character_tests` checks each of these against a control that animates from the input instead.

Threading and lifetime: a `LocomotionSet` holds a pointer to its `gfx::Skin` (which must outlive it) and is immutable after setup, so many controllers may share one. A controller is single-threaded state. `GroundQuery` and `ObstacleQuery` are `std::function`s; the glue in `capsule_glue.hpp` captures a `StaticCollision` by reference, which must outlive the query. Positions are the collision's chunk-local meters.

## What is in it

* `humanoid.hpp`: `Bone` roles and `map_humanoid`, by name (Rigify `DEF-`, Unreal, Mixamo, CMU all map; case, prefixes, separators and L/R suffixes are ignored; core bones are required). Needs `Skin::joint_names` (new in the glTF importer).
* `pose_ops.hpp`: model-space joint transforms, pose blending, pose distance.
* `clip_analysis.hpp`: measures a locomotion cycle from its poses instead of trusting its name: the **ground speed** (how fast the character must move for planted feet not to slide), stride, when each foot is planted, loop quality, and the rig's facing and proportions. The contact point is the toe joint (an ankle drifts through the heel-to-toe roll), the speed is the median over the interior of the planted run, and the planted window is refined to where the foot moves at that speed.
* `locomotion.hpp`: `LocomotionSet` (cycles sorted by measured speed, idle, jump start, air, land) and `LocomotionController`: states Ground / JumpStart / Air / Land with crossfades; the ground state blends the two cycles around the speed with **stride-matched playback** (cycle rate = speed / blended stride, so the blended planted foot moves at the character's speed) and **one shared phase** aligned on the left-foot landing of every cycle; idle with hysteresis and a fixed-length crossfade.
* `bvh.hpp`: a BVH importer (CMU motion capture) and `extract_loop`, which finds the best repeating stretch of a capture and closes it into a seamless in-place cycle (facing +X, floor and origin normalized).
* `retarget.hpp`: world-orientation retargeting between humanoids of different proportions, rest poses, names and facing; hips translation scaled by the leg-length ratio.
* `ik.hpp`: analytic two-bone IK, ground adaptation (pelvis height, per-leg IK, foot tilt to the surface normal) and wall avoidance.
* `capsule_glue.hpp`: capsule state to locomotion input, ray-cast ground and obstacle queries.

## Assets

| Asset | Use | License | Notes |
| --- | --- | --- | --- |
| Quaternius Universal Animation Library (Standard, Godot): one rig with a mesh, 46 clips | the humanoid rig; idle, walk, sprint, jump clips | CC0 | **keyframed by artists, not motion capture**; the walk runs at 1.05 m/s and the sprint at 8.96 m/s by measurement, with nothing between |
| CMU Graphics Lab motion capture (BVH conversion by B. Hahne) | real human walk, brisk walk and jogging clips, retargeted to the rig; fills the speed range between the UAL's walk and sprint | free for research and commercial use; attribution text requested (in the catalog and `tools/animation/assets.json`) | raw redistribution is not stated, so files are fetched by script and not committed |
| Quaternius UAL 2 (Unreal rig), KayKit Character Animations | cataloged, not used | CC0 | UAL 2 is the same family on an Unreal skeleton (maps with the same code); KayKit's rig has 6 joints and is not humanoid enough for locomotion |

`python3 tools/animation/fetch_humanoid_assets.py DIR` downloads and verifies (SHA-256) the CMU files and copies the UAL file from the asset cache; `SAIGE_ANIM_ASSETS=DIR ctest -R animation_assets` then runs the real-asset test (exit 77 / skipped without it).

## Evidence

Software only; no physical GPU is involved (this is CPU animation).

* Clip analysis against the independent truth: on synthetic gaits with exact foot paths the measured ground speed is exact (1.000 for 1.0, 3.500 for 3.5 m/s); on the CMU captures the foot-based speed agrees with the actor's own travel speed (root travel over the cycle) to within 5%: walks 1.19 / 1.39 / 1.88 m/s, jogs 2.7 to 2.8, runs 3.6 to 4.0 m/s.
* Foot sliding (planted contact points, mean world speed): at the cycles' own speeds under 6 cm/s on synthetic gaits (0.1 to 0.8% of the speed); between two gaits, 0.1 to 0.3 m/s (the blended stance windows only partly overlap); the control without stride matching slides 2.0 m/s beyond the fastest cycle. On the real set: the UAL sprint 2.2% of the speed, the retargeted capture cycles 10 to 28% (see Limitations).
* Transitions: the largest per-tick pose change through a jump is below the fastest gait's own per-tick change; every state sequence (walk, jump, fall, land; walking off a ledge; a hop that lands during the takeoff; standing landing) is checked.
* Collision consistency, with Core's `CapsuleCharacter` on static boxes and triangles: running into a wall, the character stops and returns to idle (idle weight above 0.95) while a control driven by the input keeps running (below 0.05); with wall IK no foot goes more than 5 mm into the wall, without it a running pose leans its feet more than 5 cm into it; on 10 and 20 degree ramps planted feet are within 8 and 24 mm of the surface with ground IK and more than 6 cm off without it; the jump states start within two ticks of the capsule's takeoff and landing.
* Each guard was shown to fail against its defect (removing stride matching, the phase alignment, the ground IK, the wall IK).

## Limitations and next steps

* The retargeted capture cycles keep the capture's own toe motion through the roll of the foot, plus retargeting's proportion error, so their planted feet move at 10 to 28% of the speed. **Foot locking** (pinning a planted foot's world position for the length of its stance with the two-bone IK) would remove it. Backlog.
* The UAL's walk (1.05 m/s) and sprint (8.96 m/s) are far apart and its jog is missing; the capture cycles fill the gap, but a character's speed range should be decided with the animator.
* No strafing, turning in place, or crouching; no root-motion mode for clips that carry the character (roll, climb); no second jump while airborne; no upper-body layering (aim, carry); no hand IK; no ragdoll. All are backlog items.
* IK is two-bone and analytic; the foot is tilted to the ground normal up to a limit, and the toes do not bend.
* Facing is the model transform's job: the controller assumes the character moves along its facing.
