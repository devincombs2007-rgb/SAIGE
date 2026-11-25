# Ragdolls and motor-driven animation (CHAR-2 foundations)

`include/saige/ragdoll.hpp` builds a humanoid ragdoll on the dynamics interface (`docs/physics-backends.md`), independent of the animation module: a pose is thirteen world transforms
(`RagdollPose`) and mapping a skeleton's joints onto the bones is the caller's (the animation glue; **not done yet for the UAL rig**). Needs a dynamics backend (`SAIGE_PHYSICS_JOLT=ON`); the tests skip otherwise.

* **Body.** Thirteen bodies (pelvis, torso, head, upper and lower arms, thighs, shins, feet; no hands or fingers yet) from `standard_humanoid(height, mass, feet, forward)`, de Leva-like mass fractions, centre of mass at 57% of the height
  standing. Bodies of one ragdoll do not collide with each other (`self_collision_group`), only with the world.
* **Anatomical joints.** Hinges for knees (flexion 0 to 2.4 rad, never hyperextended) and elbows (0 to 2.5 rad), swing-twist cones for spine, neck, shoulders, hips and ankles, all in `RagdollLimits`.
* **Animation to ragdoll** (`start(now, before, dt)`): places every body at the animated pose and gives it the linear and angular velocity that took the previous pose to this one, so momentum is continuous across the switch.
* **Ragdoll to animation** (`RagdollRecovery`): a smoothstep weight over a duration, and a blend of the physical and animated poses (positions linear, rotations by shortest-arc slerp); `lying_face_up()` picks which get-up to play.
* **Motor-driven physical animation** (`drive_to_pose`): a spring-damper motor on every joint toward the animated pose, solved with the constraints (stable at any stiffness, works under load). `balance` is an ankle strategy on top.

## Measured (Jolt 5.7.0, Linux GCC, single precision; the double build and Debug under ASan/UBSan pass the same tests)

| Test | Result |
| --- | --- |
| Limp collapse from a stand (6 s) | comes to rest (0.04 m/s) low (center of mass 0.11 m), nothing below the floor (lowest 0.026 m); knees never bend the wrong way (-0.00 rad); elbows hyperextend at most 0.07 rad |
| Joint limits under the body's weight | soft: knee flexion up to 2.48 rad against a 2.4 limit, elbows 2.73 against 2.5 (4 substeps, 10 position iterations; with 2 substeps and 2 iterations the overshoot was 0.38 rad) |
| Momentum from the animation | started at 4 m/s forward and 1 m/s up: (300.0, 0.0, 75.0) kg m/s as asked; after 0.5 s of flight horizontal momentum conserved (297.0 with the 2%/s damping) and vertical follows gravity; a pose turning at 1.2 rad/s starts the pelvis at 1.200 rad/s |
| Fall from 3 m | speed 7.46 m/s before contact, free fall 7.46; nothing through the floor (-0.003 m) |
| Crate (20 kg, 12 m/s) into a ragdoll in the air | momentum 240.0 before, 236.9 after (damping), ragdoll carried at 2.5 m/s |
| Face up / down | correct for a 90 degree tip either way |
| Motor tracking in zero gravity | 15 Hz, damping 2: joint error 1.05 rad at the start, 0.34 after 1 s, 0.003 after 8 s (at most 0.023 between 5 and 8 s) |
| Standing | stiff motors alone **do not** keep it up: a joint motor's stiffness follows the reduced mass of the two bodies it joins (a foot against a shin), not the weight above, so the ankle spring is a few hundred N m/rad against the 735 N m/rad an inverted pendulum of 75 kg and 1 m needs; it stands a moment and sways over like a plank |
| Ankle strategy (`balance`) | tilts the feet's targets against the center of mass' offset and velocity: stands steadily (0.09 m ahead of the feet's centre), recovers from pushes of 15 and 22 N s (0.2 and 0.29 m/s of the body) from behind, the front, and both sides; a 60 N s push (0.8 m/s) topples it, as it must: the capture point is past the toe |

Mutation checks (22 across the ragdoll and the new adapter paths: limits swapped, velocities dropped, face-up inverted, recovery weight, blend, balance signs, drive target, support scale, centre of mass, self-collision group, set_transform, motor target, motor not on, wake, angular impulse, sleeping) are each killed; two equivalent ones were removed (Jolt already wakes a body on an impulse).

## The skeleton to and from the ragdoll (`engine/animation/include/saige/animation/ragdoll_glue.hpp`)

`spec_from_pose` measures the body in any pose from the humanoid bone map (the model space is SAIGE's, +X forward, +Y left, +Z up, so a pose reaches the world by a translation and a turn about +Z: `RagdollFrame`); `humanoid_body_pose(spec)` gives the
thirteen body transforms of that pose (what `Ragdoll::start` takes; a ragdoll is built from the rest pose and started from the live one). `pose_from_ragdoll` makes the skeleton follow the bodies: each joint is turned by the rotation its body
has turned since the reference pose (spine and clavicles take the torso's, the neck the middle of torso and head, hands the forearm's, toes the foot's), parents first, and the hips are moved to the pelvis, so bone lengths are the rig's exactly.
Measured on the synthetic humanoid with Jolt (`saige_animation_ragdoll_test`): bodies of a pose equal the ragdoll's rest pose; start and read back changes the pose by 0.00000 rad and 0.00000 m; after a 5 s collapse the skeleton's ankles and wrists are
within 2.5 cm of the ends of the shins and forearms, feet, hands, head and neck turn exactly as their bodies, the hips are at the pelvis, and bone lengths differ from the rig's by 6e-8 m. Seven mutations are each killed (one real bug found on the way:
a joint's turn was applied to a rotation its parent's turn had already changed). **Not run on the real UAL rig yet** (the mapping uses the bone map, which handles it, but only the synthetic rig is tested).

## Not done (honest list)

* Stepping: balance recovery by taking a step (the capture-point strategy beyond the foot), falls with protective arms, get-up animations and their selection by `lying_face_up`.
* A transition test on the real UAL rig (the glue exists and is tested on the synthetic one), hands and fingers, and tuning limits against a real rig's range of motion.
* Torque limits per joint from muscle strength (the API has `max_torque`, unused by default); load-bearing joints run at `support_scale` times the motor frequency, a pragmatic stand-in for proper contact-aware control.
* Impact-reactive behaviors (the Euphoria direction: stumble, brace, grab): this is the substrate they would run on; none exists yet.
* The vehicle entry and exit transitions and doors, strafing and turn-in-place animations from the CMU candidates (candidates are recorded, not yet reviewed by eye), smooth crouch transitions.
