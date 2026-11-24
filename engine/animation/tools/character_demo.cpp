// A small runnable example of the whole chain: a capsule character (Core's controller on static collision) runs a scripted course (flat ground, a jump, a ramp, a wall) while a locomotion controller
// animates a humanoid from the capsule's real speed, ground IK plants its feet on the ramp and wall IK keeps them out of the wall. It prints a line every half second.
// Usage: saige_anim_character_demo ASSET_DIR      (ASSET_DIR from tools/animation/fetch_humanoid_assets.py)
#include "saige/animation/bvh.hpp"
#include "saige/animation/capsule_glue.hpp"
#include "saige/animation/retarget.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace saige;
using namespace saige::anim;

namespace {

const gfx::AnimationClip* find_clip(const gfx::GltfAsset& a, const char* name) {
    for (const gfx::AnimationClip& c : a.animations) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const char* state_name(LocomotionState s) {
    switch (s) {
        case LocomotionState::Ground: return "ground";
        case LocomotionState::JumpStart: return "takeoff";
        case LocomotionState::Air: return "air";
        default: return "landing";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: saige_anim_character_demo ASSET_DIR\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const gfx::GltfLoadResult loaded = gfx::load_gltf_file(root / "ual" / "AnimationLibrary_Godot_Standard.glb");
    if (!loaded.ok()) {
        std::fprintf(stderr, "cannot load the rig: %s\n", loaded.error.c_str());
        return 1;
    }
    const gfx::GltfAsset& ual = *loaded.asset;
    const gfx::Skin& skin = ual.skins[0];
    const auto map = map_humanoid(skin);
    if (!map) return 1;
    const RigInfo rig = analyze_rig(skin, *map);
    LocomotionSet set(skin, *map);
    for (const char* id : {"07_01", "07_12", "16_35", "16_45"}) {
        std::ifstream in(root / "cmu" / (std::string(id) + ".bvh"));
        std::stringstream ss;
        ss << in.rdbuf();
        BvhOptions opt;
        opt.unit_scale = cmu_unit_scale;
        const BvhResult bvh = parse_bvh(ss.str(), opt);
        if (!bvh.ok()) continue;
        const auto smap = map_humanoid(*bvh.skin);
        if (!smap) continue;
        const RigInfo srig = analyze_rig(*bvh.skin, *smap);
        const LoopResult loop = extract_loop(*bvh.skin, *smap, srig, *bvh.clip);
        if (!loop.clip) continue;
        const gfx::Pose srest = gfx::rest_pose(*bvh.skin);
        set.add_cycle(retarget_clip({&*bvh.skin, &*smap, &srig, &srest}, *loop.clip, skin, *map, rig));
    }
    if (const gfx::AnimationClip* c = find_clip(ual, "Sprint_Loop")) set.add_cycle(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Idle_Loop")) set.set_idle(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Start")) set.set_jump_start(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Loop")) set.set_air(*c);
    if (const gfx::AnimationClip* c = find_clip(ual, "Jump_Land")) set.set_land(*c);
    if (!set.usable()) return 1;
    std::printf("%zu locomotion cycles:", set.cycles().size());
    for (const LocomotionCycle& c : set.cycles()) std::printf(" %.2f m/s", static_cast<double>(c.info.ground_speed));
    std::printf("\n");

    // The course: a floor, a ramp from x = 20 to 28 (12 degrees), then a plateau, and a wall at x = 60.
    const float rise = 8.0f * 0.2126f;
    std::vector<Aabb> boxes = {{{-50, -20, -1}, {200, 20, 0}}, {{28, -20, -1}, {200, 20, rise}}, {{60, -20, -1}, {61, 20, 20}}};
    std::vector<Triangle> tris = {{{20, -20, 0}, {20, 20, 0}, {28, 20, rise}}, {{20, -20, 0}, {28, 20, rise}, {28, -20, rise}}};
    const StaticCollision world(boxes, tris);
    CapsuleCharacter character({0.0f, 0.0f, 0.0f}, boxes, tris, {});
    LocomotionController anim(set, {});
    FootIkState ik;
    const GroundQuery ground = ground_query(world);
    const ObstacleQuery obstacle = obstacle_query(world);
    const std::chrono::nanoseconds dt{16666667};
    for (int i = 0; i < 60 * 24; ++i) {
        const float t = static_cast<float>(i) / 60.0f;
        CapsuleInput in{1.0f, 0.0f, false, t > 3.0f};  // walk, then sprint from 3 s
        if (t < 1.0f) in.forward = 0.0f;                // stand for a second
        in.jump = i == 60 * 6;
        character.step(in, dt);
        const CapsuleCharacterState& s = character.state();
        anim.update(1.0f / 60.0f, locomotion_input(s));
        gfx::Pose pose = anim.pose();
        const render::Transform rt{{s.feet.x, s.feet.y, s.feet.z}, {0, 0, 0, 1}, {1, 1, 1}};
        if (s.grounded && anim.state() == LocomotionState::Ground) adapt_feet_to_ground(skin, *map, rig, pose, rt, ground, {}, ik, 1.0f / 60.0f);
        const int moved = keep_feet_outside_obstacles(skin, *map, pose, rt, obstacle, {});
        if (i % 30 == 0) {
            std::printf("t=%5.1f s  x=%6.2f z=%5.2f  speed=%5.2f  %-7s idle=%.2f rate=%.2f c/s  pelvis %+.2f%s\n", static_cast<double>(t), static_cast<double>(s.feet.x), static_cast<double>(s.feet.z),
                        static_cast<double>(std::hypot(s.velocity.x, s.velocity.y)), state_name(anim.state()), static_cast<double>(anim.idle_weight()), static_cast<double>(anim.cycle_rate()),
                        static_cast<double>(ik.pelvis_offset), moved > 0 ? "  (foot kept out of the wall)" : "");
        }
    }
    return 0;
}
