// The animation module on real assets: Quaternius' Universal Animation Library rig and clips (CC0) and CMU motion capture (BVH), retargeted onto that rig. Skipped (exit 77) when the assets are not
// fetched: run tools/animation/fetch_humanoid_assets.py DIR and pass DIR as argv[1] or in SAIGE_ANIM_ASSETS.
#include "saige/animation/action.hpp"
#include "saige/animation/bvh.hpp"
#include "saige/animation/ik.hpp"
#include "saige/animation/layers.hpp"
#include "saige/animation/locomotion.hpp"
#include "saige/animation/retarget.hpp"

#include "metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace saige;
using namespace saige::anim;

namespace {

int failures = 0;

void check(bool ok, const std::string& what, double value = 0.0) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (%.4f)\n", what.c_str(), value);
        ++failures;
    }
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const gfx::AnimationClip* find_clip(const gfx::GltfAsset& a, const std::string& name) {
    for (const gfx::AnimationClip& c : a.animations) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    const char* env = std::getenv("SAIGE_ANIM_ASSETS");
    const std::filesystem::path root = argc > 1 ? argv[1] : env != nullptr ? env : "";
    const auto ual_path = root / "ual" / "AnimationLibrary_Godot_Standard.glb";
    if (root.empty() || !std::filesystem::exists(ual_path) || !std::filesystem::exists(root / "cmu")) {
        std::printf("humanoid assets not available (tools/animation/fetch_humanoid_assets.py DIR; pass DIR or set SAIGE_ANIM_ASSETS): skipped\n");
        return 77;
    }
    const gfx::GltfLoadResult loaded = gfx::load_gltf_file(ual_path);
    if (!loaded.ok() || loaded.asset->skins.empty()) {
        std::fprintf(stderr, "FAIL: the UAL rig does not load: %s\n", loaded.error.c_str());
        return 1;
    }
    const gfx::GltfAsset& ual = *loaded.asset;
    const gfx::Skin& skin = ual.skins[0];
    std::string why;
    const auto map = map_humanoid(skin, &why);
    check(map.has_value(), "the UAL rig maps to a humanoid: " + why);
    if (!map) return 1;
    const RigInfo rig = analyze_rig(skin, *map);
    std::printf("UAL rig: %zu joints, %zu clips, facing (%.2f %.2f), hips %.3f m, leg %.3f m\n", skin.joint_count(), ual.animations.size(), static_cast<double>(rig.forward.x), static_cast<double>(rig.forward.y),
                static_cast<double>(rig.hips_height), static_cast<double>(rig.leg_length));
    check(std::fabs(rig.forward.x - 1.0f) < 0.02f && std::fabs(rig.forward.y) < 0.02f, "the UAL rig faces +X");
    check(rig.hips_height > 0.7f && rig.hips_height < 1.0f, "plausible hips height", static_cast<double>(rig.hips_height));

    // The UAL's own clips.
    LocomotionSet set(skin, *map);
    struct Expect {
        const char* name;
        float lo, hi;  // plausible ground speed, m/s
    };
    for (const Expect e : {Expect{"Walk_Loop", 0.8f, 1.5f}, Expect{"Sprint_Loop", 7.0f, 11.0f}}) {
        const gfx::AnimationClip* c = find_clip(ual, e.name);
        check(c != nullptr, std::string("clip ") + e.name);
        if (c == nullptr) continue;
        const ClipInfo ci = analyze_cycle(skin, *map, rig, *c);
        std::printf("  %-12s ground %.2f m/s stride %.2f m loop_err %.3f\n", e.name, static_cast<double>(ci.ground_speed), static_cast<double>(ci.stride_length), static_cast<double>(ci.loop_error));
        check(ci.valid && ci.ground_speed > e.lo && ci.ground_speed < e.hi, std::string(e.name) + " ground speed in its plausible range", static_cast<double>(ci.ground_speed));
        check(ci.loop_error < 0.05f, std::string(e.name) + " loops", static_cast<double>(ci.loop_error));
    }
    for (const char* n : {"Idle_Loop", "Jump_Start", "Jump_Loop", "Jump_Land"}) check(find_clip(ual, n) != nullptr, std::string("clip ") + n);

    // The root-motion clips (the "_RM" variants carry the travel; the plain "Roll" is the same motion in place): an action measures their travel, commands it step by step, and plays the pose in place.
    {
        const std::size_t hips = static_cast<std::size_t>((*map)[Bone::Hips]);
        struct Rm {
            const char* name;
            float lo, hi;  // plausible travel, meters
        };
        for (const Rm r : {Rm{"Roll_RM", 4.0f, 6.0f}, Rm{"Sword_Attack_RM", 0.8f, 2.0f}, Rm{"Roll", 0.0f, 0.3f}}) {
            const gfx::AnimationClip* c = find_clip(ual, r.name);
            check(c != nullptr, std::string("clip ") + r.name);
            if (c == nullptr) continue;
            const ActionClip action(skin, *map, rig, *c);
            ActionPlayer player;
            player.start(action);
            const render::Float3 start_hips = joint_world(skin, player.pose())[hips].position;  // where the clip's first frame has the hips (not the rest pose's)
            float commanded = 0.0f, swim = 0.0f;
            int steps = 0;
            while (player.active() && steps < 900) {
                commanded += player.command(1.0f / 60.0f).forward / 60.0f;
                player.advance(1.0f / 60.0f, 1.0f);
                const render::Float3 h = joint_world(skin, player.pose())[hips].position;
                swim = std::max(swim, std::hypot(h.x - start_hips.x, h.y - start_hips.y));
                ++steps;
            }
            std::printf("  %-16s action: travel %.2f m forward (%.2f m total), commanded %.2f m, body %.1f mm from its start over %d ticks\n", r.name, static_cast<double>(action.travel().forward),
                        static_cast<double>(action.travel_length()), static_cast<double>(commanded), static_cast<double>(swim) * 1000.0, steps);
            check(action.valid() && action.travel_length() >= r.lo && action.travel_length() <= r.hi, std::string(r.name) + " travel in its plausible range", static_cast<double>(action.travel_length()));
            check(std::fabs(commanded - action.travel().forward) < 0.01f, std::string(r.name) + " commands exactly the travel it measured", static_cast<double>(commanded));
            // Hips travel in the model's own frame can swing a little off the heading during a roll; the in-place pose must still hold the body at the origin to within a few millimeters.
            check(swim < 0.005f, std::string(r.name) + " is played in place", static_cast<double>(swim));
        }
    }

    // The mocap: import, loop, retarget, and compare with what the capture itself says.
    const gfx::Pose urest = gfx::rest_pose(skin);
    struct Mocap {
        const char* id;
        bool use;
    };
    std::vector<gfx::AnimationClip> cycles;
    for (const Mocap m : {Mocap{"07_01", true}, Mocap{"07_12", true}, Mocap{"16_35", true}, Mocap{"16_45", true}, Mocap{"09_01", false}, Mocap{"02_03", false}}) {
        const auto path = root / "cmu" / (std::string(m.id) + ".bvh");
        check(std::filesystem::exists(path), std::string("fetched ") + m.id);
        if (!std::filesystem::exists(path)) continue;
        BvhOptions opt;
        opt.unit_scale = cmu_unit_scale;
        const BvhResult bvh = parse_bvh(slurp(path), opt);
        check(bvh.ok(), std::string(m.id) + " parses: " + bvh.error);
        if (!bvh.ok()) continue;
        std::string w2;
        const auto smap = map_humanoid(*bvh.skin, &w2);
        check(smap.has_value(), std::string(m.id) + " maps to a humanoid: " + w2);
        if (!smap) continue;
        const RigInfo srig = analyze_rig(*bvh.skin, *smap);
        check(std::fabs(srig.hips_height - 0.0f) > 0.5f, "the capture skeleton has a hips height", static_cast<double>(srig.hips_height));
        const LoopResult loop = extract_loop(*bvh.skin, *smap, srig, *bvh.clip);
        check(loop.clip.has_value(), std::string(m.id) + " has a repeating stretch: " + loop.error);
        if (!loop.clip) continue;
        const gfx::Pose srest = gfx::rest_pose(*bvh.skin);
        const RetargetSource rs{&*bvh.skin, &*smap, &srig, &srest};
        const gfx::AnimationClip rt = retarget_clip(rs, *loop.clip, skin, *map, rig);
        const ClipInfo ti = analyze_cycle(skin, *map, rig, rt);
        const float ratio = rig.leg_length / srig.leg_length;
        const float travel_speed = loop.travel / loop.clip->duration;  // the actor's own speed in the capture, an independent measure
        std::printf("  %s: cycle %.2f s, the actor moved %.2f m/s; retargeted needs %.2f m/s (expected %.2f from the leg ratio %.2f), stride %.2f m, loop_err %.3f, seam %.3f rad\n", m.id,
                    static_cast<double>(loop.clip->duration), static_cast<double>(travel_speed), static_cast<double>(ti.ground_speed), static_cast<double>(travel_speed * ratio), static_cast<double>(ratio),
                    static_cast<double>(ti.stride_length), static_cast<double>(ti.loop_error), static_cast<double>(loop.seam_error));
        check(loop.clip->duration > 0.45f && loop.clip->duration < 1.8f, std::string(m.id) + " cycle length", static_cast<double>(loop.clip->duration));
        check(loop.seam_error < 0.7f, std::string(m.id) + " seam before closing", static_cast<double>(loop.seam_error));
        check(ti.valid, std::string(m.id) + " retargeted clip is a locomotion cycle");
        check(std::fabs(ti.ground_speed - travel_speed * ratio) < 0.12f * travel_speed * ratio + 0.05f, std::string(m.id) + " retargeted speed follows the capture's own speed", static_cast<double>(ti.ground_speed));
        check(ti.loop_error < 0.05f, std::string(m.id) + " retargeted loop is seamless", static_cast<double>(ti.loop_error));
        // The feet reach the floor and do not sink through it.
        float lowest = 1e9f;
        const gfx::ClipBinding b = gfx::bind_clip(rt, skin);
        for (int k = 0; k < 60; ++k) {
            gfx::Pose p = urest;
            gfx::evaluate_clip(rt, b, rt.duration * static_cast<float>(k) / 60.0f, true, p);
            const auto jw = joint_world(skin, p);
            for (const Bone t : {Bone::LeftToes, Bone::RightToes}) lowest = std::min(lowest, jw[static_cast<std::size_t>((*map)[t])].position.z);
        }
        check(std::fabs(lowest) < 0.05f, std::string(m.id) + " the feet reach the floor without sinking through it", static_cast<double>(lowest));
        if (m.use) cycles.push_back(rt);
    }

    // A locomotion set from real data: three mocap gaits plus the UAL's sprint, idle and jump clips.
    for (const gfx::AnimationClip& c : cycles) {
        std::string w3;
        check(set.add_cycle(c, &w3), "a mocap cycle is accepted by the set: " + w3);
    }
    if (const gfx::AnimationClip* c = find_clip(ual, "Sprint_Loop")) check(set.add_cycle(*c), "UAL sprint accepted");
    if (const gfx::AnimationClip* c = find_clip(ual, "Idle_Loop")) set.set_idle(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Start")) set.set_jump_start(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Loop")) set.set_air(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Land")) set.set_land(*c);
    check(set.cycles().size() >= 4, "the set has its cycles", static_cast<double>(set.cycles().size()));
    std::printf("the set's cycles (m/s):");
    for (const LocomotionCycle& c : set.cycles()) std::printf(" %.2f", static_cast<double>(c.info.ground_speed));
    std::printf("\n");
    // Crouching on the real rig: UAL's crouch idle and crouch walk as their own set, the foot contact at the cycle's own speed, and the capsule height that matches the pose.
    {
        LocomotionSet crouch(skin, *map);
        const gfx::AnimationClip* cf = find_clip(ual, "Crouch_Fwd_Loop");
        const gfx::AnimationClip* ci = find_clip(ual, "Crouch_Idle_Loop");
        check(cf != nullptr && ci != nullptr, "the crouch clips exist");
        if (cf != nullptr && ci != nullptr) {
            std::string why_not;
            check(crouch.add_cycle(*cf, &why_not), "the crouch walk is a locomotion cycle: " + why_not);
            crouch.set_idle(*ci);
            if (crouch.usable()) {
                const float v = crouch.cycles()[0].info.ground_speed;
                const double rel = metrics::window_slide(crouch, LocomotionConfig{}, 0, v);
                std::printf("  crouch walk: %.2f m/s, planted feet move at %.1f%% of the speed\n", static_cast<double>(v), rel * 100.0);
                check(rel < 0.08, "the crouch walk's planted feet hardly slide at its own speed", rel);
                // The head's top over the crouch clips against the capsule height it gets.
                float head_max = 0.0f;
                for (const gfx::AnimationClip* c : {cf, ci}) {
                    const gfx::ClipBinding bind = gfx::bind_clip(*c, skin);
                    gfx::Pose p = gfx::rest_pose(skin);
                    for (int i = 0; i < 40; ++i) {
                        gfx::evaluate_clip(*c, bind, c->duration * static_cast<float>(i) / 40.0f, true, p);
                        head_max = std::max(head_max, joint_world(skin, p)[static_cast<std::size_t>((*map)[Bone::Head])].position.z);
                    }
                }
                float standing_head = 0.0f;
                if (const gfx::AnimationClip* idle = find_clip(ual, "Idle_Loop")) {
                    gfx::Pose p = gfx::rest_pose(skin);
                    gfx::evaluate_clip(*idle, gfx::bind_clip(*idle, skin), 0.0f, true, p);
                    standing_head = joint_world(skin, p)[static_cast<std::size_t>((*map)[Bone::Head])].position.z;
                }
                const float capsule = crouch_capsule_height(head_max, standing_head, 1.7f, 0.6f);
                std::printf("  crouch head joint tops out at %.2f m (standing %.2f m): a crouched capsule of %.2f m (%.0f%% of 1.7 m)\n", static_cast<double>(head_max), static_cast<double>(standing_head),
                            static_cast<double>(capsule), static_cast<double>(capsule / 1.7f) * 100.0);
                check(capsule > 0.9f && capsule < 1.3f, "a plausible crouch capsule height", static_cast<double>(capsule));
                // The crouch and the standing pose crossfade without a jump: the largest joint change per tick through the blend is far below a gait's own.
                LocomotionController stand(set, {}), low(crouch, {});
                StanceBlend stance(0.2f);
                gfx::Pose previous = stand.pose();
                float worst = 0.0f;
                for (int i = 0; i < 90; ++i) {
                    stand.update(1.0f / 60.0f, {0.0f, true, 0.0f});
                    low.update(1.0f / 60.0f, {0.0f, true, 0.0f});
                    stance.update(1.0f / 60.0f, i >= 20);
                    gfx::Pose out = stand.pose();
                    blend_into(out, low.pose(), stance.weight());
                    if (i > 0) worst = std::max(worst, pose_distance(previous, out));
                    previous = out;
                }
                // The reference: how much a pose changes per tick in the fastest gait itself.
                LocomotionController sprint(set, {});
                for (int i = 0; i < 120; ++i) sprint.update(1.0f / 60.0f, {set.cycles().back().info.ground_speed, true, 0.0f});
                gfx::Pose before = sprint.pose();
                float gait = 0.0f;
                for (int i = 0; i < 60; ++i) {
                    sprint.update(1.0f / 60.0f, {set.cycles().back().info.ground_speed, true, 0.0f});
                    gait = std::max(gait, pose_distance(before, sprint.pose()));
                    before = sprint.pose();
                }
                std::printf("  largest joint rotation change per tick: %.3f rad through the stance blend, %.3f rad in the fastest gait itself\n", static_cast<double>(worst), static_cast<double>(gait));
                check(worst < gait, "the stance blend changes the pose per tick less than the fastest gait does", static_cast<double>(worst));
            }
        }
    }

    // Layering on the real rig: UAL's aim pose over the sprint, and a look-at.
    if (set.usable()) {
        const gfx::AnimationClip* aim = find_clip(ual, "Pistol_Aim_Neutral");
        check(aim != nullptr, "clip Pistol_Aim_Neutral");
        if (aim != nullptr) {
            LocomotionController run(set, {});
            for (int i = 0; i < 120; ++i) run.update(1.0f / 60.0f, {7.0f, true, 0.0f});
            const gfx::Pose base = run.pose();
            gfx::Pose layer = gfx::rest_pose(skin);
            gfx::evaluate_clip(*aim, gfx::bind_clip(*aim, skin), aim->duration, false, layer);
            const BoneMask mask = upper_body_mask(skin, *map);
            gfx::Pose out = base;
            blend_masked(out, layer, mask, 1.0f);
            bool legs_identical = true;
            for (const Bone leg : {Bone::Hips, Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes, Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes}) {
                const std::size_t j = static_cast<std::size_t>((*map)[leg]);
                legs_identical = legs_identical && out.local[j].rotation == base.local[j].rotation && out.local[j].position == base.local[j].position;
            }
            check(legs_identical, "the aim layer leaves the sprint's legs and hips bit-identical");
            float arm_to_layer = 0.0f, arm_to_base = 0.0f;
            for (const Bone arm : {Bone::LeftUpperArm, Bone::RightUpperArm, Bone::LeftLowerArm, Bone::RightLowerArm}) {
                const std::size_t j = static_cast<std::size_t>((*map)[arm]);
                arm_to_layer = std::max(arm_to_layer, rotation_angle_between(out.local[j].rotation, layer.local[j].rotation));
                arm_to_base = std::max(arm_to_base, rotation_angle_between(base.local[j].rotation, layer.local[j].rotation));
            }
            std::printf("  aim layer over the sprint: arms end %.4f rad from the aim pose (the sprint's arms were %.2f rad from it)\n", static_cast<double>(arm_to_layer), static_cast<double>(arm_to_base));
            check(arm_to_layer < 1e-4f && arm_to_base > 0.3f, "the aim layer puts the arms in the aim pose", static_cast<double>(arm_to_layer));
            // Hand IK on the UAL rig: each hand on a point in front of the chest (a door handle), the arm keeping its bone lengths.
            for (const bool left_hand : {true, false}) {
                gfx::Pose arm_pose = base;
                const render::Transform at_feet{{2, 1, 0}, render::from_axis_angle({0, 0, 1}, 0.6f), {1, 1, 1}};
                const Bone ub = left_hand ? Bone::LeftUpperArm : Bone::RightUpperArm, hb = left_hand ? Bone::LeftHand : Bone::RightHand;
                const std::vector<JointWorld> before_w = joint_world(skin, arm_pose);
                const render::Float3 shoulder = before_w[static_cast<std::size_t>((*map)[ub])].position;
                const render::Float3 handle_model = shoulder + render::Float3{0.35f, left_hand ? 0.1f : -0.1f, -0.15f};
                const render::Float3 handle_world = at_feet.position + render::rotate(at_feet.rotation, handle_model);
                const HandIkResult hr = reach_hand(skin, *map, rig, arm_pose, at_feet, left_hand, handle_world, 1.0f);
                const std::vector<JointWorld> after_w = joint_world(skin, arm_pose);
                const render::Float3 got = after_w[static_cast<std::size_t>((*map)[hb])].position;
                const float err = std::hypot(std::hypot(got.x - handle_model.x, got.y - handle_model.y), got.z - handle_model.z);
                const std::size_t lj = static_cast<std::size_t>((*map)[left_hand ? Bone::LeftLowerArm : Bone::RightLowerArm]);
                const float len0 = render::length(before_w[lj].position - before_w[static_cast<std::size_t>((*map)[ub])].position), len1 = render::length(after_w[lj].position - after_w[static_cast<std::size_t>((*map)[ub])].position);
                std::printf("  UAL %s hand to a point 0.35 m ahead of the shoulder: %.2f mm off, reached=%d\n", left_hand ? "left" : "right", static_cast<double>(err) * 1000.0, hr.reached ? 1 : 0);
                check(hr.reached && err < 0.003f, "the UAL hand reaches the point", static_cast<double>(err));
                check(std::fabs(len0 - len1) < 1e-4f, "the upper arm keeps its length", static_cast<double>(len1 - len0));
            }
            // Look at a point 4 m ahead and 2 m to the left of the standing character, at head height.
            gfx::Pose stand = gfx::rest_pose(skin);
            const render::Transform at_origin{{0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}};
            const render::Float3 eye = joint_world(skin, stand)[static_cast<std::size_t>((*map)[Bone::Head])].position;
            LookAtState state;
            const render::Float3 target = eye + render::Float3{4.0f, 2.0f, 0.0f};
            const float left = look_at(skin, *map, rig, stand, at_origin, target, {}, state, 1.0f, 1.0f / 60.0f);
            std::printf("  look at (4, 2) m from the head: %.2f degrees off after the turn\n", static_cast<double>(left) * 57.29578);
            check(left < 0.035f, "the UAL head faces the target within 2 degrees", static_cast<double>(left));
        }
    }
    if (set.usable()) {
        LocomotionConfig cfg;
        // Slide inside the stance of each cycle, at the cycle's own speed (a pure cycle): the planted feet move at a few percent of the speed. Between cycles the blends are not pure and are reported only.
        double worst_rel = 0.0, worst_locked = 0.0;
        for (std::size_t i = 0; i < set.cycles().size(); ++i) {
            const float v = set.cycles()[i].info.ground_speed;
            const double rel = metrics::window_slide(set, cfg, i, v);
            std::printf("  cycle %zu at %.2f m/s: planted feet move at %.1f%% of the speed\n", i, static_cast<double>(v), rel * 100.0);
            worst_rel = std::max(worst_rel, rel);
            const double locked = metrics::window_slide(set, cfg, i, v, 8.0f, 2.0f, true);
            std::printf("    with foot locking: %.1f%%\n", locked * 100.0);
            worst_locked = std::max(worst_locked, locked);
            if (i + 1 == set.cycles().size()) check(rel < 0.08, "the UAL's own sprint (authored in place, feet planted): planted feet move at under 8% of the speed", rel);
        }
        // The retargeted capture cycles keep the capture's own toe motion through the roll of the foot, plus the retargeting's proportion error, so they are looser: this is the residual that foot locking
        // (planting a foot's world position for the length of its stance) would remove; it is in the backlog.
        check(worst_rel < 0.35, "real cycles: planted feet move at under 35% of the speed at the cycles' own speeds", worst_rel);
        check(worst_locked < 0.6 * worst_rel, "foot locking cuts the worst planted-foot motion by at least 40%", worst_locked);
        LocomotionConfig naive;
        naive.stride_matching = false;
        const std::size_t top_i = set.cycles().size() - 1;
        const float top = set.cycles().back().info.ground_speed * 1.8f;  // beyond the fastest cycle: played at the maximum rate scale, or authored when naive
        const double fast = metrics::window_slide(set, cfg, top_i, top);
        const double fast_naive = metrics::window_slide(set, naive, top_i, top);
        std::printf("  beyond the fastest cycle (%.2f m/s): matched %.1f%%, naive %.1f%% of the speed\n", static_cast<double>(top), fast * 100.0, fast_naive * 100.0);
        check(fast_naive > 1.5 * fast + 0.1, "the control without stride matching slides far more", fast_naive);
        // A jump on real clips: continuous, with the states in order.
        LocomotionController c(set, cfg);
        c.update(1.0f / 60.0f, {3.0f, true, 0.0f});
        gfx::Pose last = c.pose();
        float worst = 0.0f;
        std::vector<LocomotionState> seen;
        const auto step = [&](float speed, bool grounded, float vy) {
            c.update(1.0f / 60.0f, {speed, grounded, vy});
            worst = std::max(worst, pose_distance(c.pose(), last));
            last = c.pose();
            if (seen.empty() || seen.back() != c.state()) seen.push_back(c.state());
        };
        for (int i = 0; i < 90; ++i) step(3.0f, true, 0.0f);
        for (int i = 0; i < 25; ++i) step(3.0f, false, 4.4f - 9.8f * static_cast<float>(i) / 60.0f);
        for (int i = 0; i < 30; ++i) step(3.0f, false, -3.0f);
        for (int i = 0; i < 90; ++i) step(3.0f, true, 0.0f);
        const std::vector<LocomotionState> want = {LocomotionState::Ground, LocomotionState::JumpStart, LocomotionState::Air, LocomotionState::Land, LocomotionState::Ground};
        check(seen == want, "jump states in order on the real clips");
        const metrics::Slide steady = metrics::measure_slide(set, cfg, [&](float) { return top; }, 3.0f, 1.0f);
        std::printf("  largest pose change in a tick through a jump: %.3f rad (the fastest gait alone: %.3f)\n", static_cast<double>(worst), steady.max_step);
        check(static_cast<double>(worst) < steady.max_step * 1.6 + 0.1, "no pop through a jump on real clips", static_cast<double>(worst));

        // Ground IK on the real rig, on a 15 degree slope.
        const gfx::AnimationClip& walk = set.cycles().front().clip;
        const gfx::ClipBinding wb = gfx::bind_clip(walk, skin);
        double worst_gap = 0.0;
        FootIkState ik;
        FootIkConfig icfg;
        icfg.pelvis_smoothing = 0.0f;
        for (int k = 0; k < 30; ++k) {
            gfx::Pose p = urest;
            gfx::evaluate_clip(walk, wb, walk.duration * static_cast<float>(k) / 30.0f, true, p);
            const gfx::Pose raw = p;
            const float slope = 0.268f;
            adapt_feet_to_ground(skin, *map, rig, p, {{0, 0, 0}, {0, 0, 0, 1}}, [&](float x, float, float, GroundSample& g) {
                g.height = slope * x;
                g.normal = {-slope / std::sqrt(1 + slope * slope), 0.0f, 1.0f / std::sqrt(1 + slope * slope)};
                return true;
            }, icfg, ik, 1.0f / 60.0f);
            const auto jw = joint_world(skin, p), jr = joint_world(skin, raw);
            for (const Bone t : {Bone::LeftToes, Bone::RightToes}) {
                const std::size_t j = static_cast<std::size_t>((*map)[t]);
                if (jr[j].position.z > 0.01f + rig.contact_offset) continue;  // not planted by the animation
                worst_gap = std::max(worst_gap, static_cast<double>(std::fabs(jw[j].position.z - slope * jw[j].position.x)));
            }
        }
        std::printf("  ground IK on a 15 degree slope: worst planted gap %.3f m\n", worst_gap);
        check(worst_gap < 0.03, "ground IK plants real feet on a slope", worst_gap);
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
