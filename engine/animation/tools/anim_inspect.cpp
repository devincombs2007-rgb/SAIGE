// Prints what the animation module measures from a glTF rig and its clips: the humanoid mapping, the facing, and for each clip named on the command line (or every clip) the cycle analysis.
// Usage: saige_anim_inspect FILE.glb [clip ...]
#include "saige/animation/clip_analysis.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace saige;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: saige_anim_inspect FILE.glb [clip ...]\n");
        return 2;
    }
    const gfx::GltfLoadResult loaded = gfx::load_gltf_file(argv[1]);
    if (!loaded.ok()) {
        std::fprintf(stderr, "load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const gfx::GltfAsset& asset = *loaded.asset;
    if (asset.skins.empty()) {
        std::fprintf(stderr, "no skin\n");
        return 1;
    }
    const gfx::Skin& skin = asset.skins[0];
    std::string why;
    const auto map = anim::map_humanoid(skin, &why);
    if (!map) {
        std::fprintf(stderr, "not a humanoid: %s\n", why.c_str());
        return 1;
    }
    const anim::RigInfo rig = anim::analyze_rig(skin, *map);
    std::printf("%zu joints, %zu clips; forward (%.2f %.2f), hips %.3f m, leg %.3f m\n", skin.joint_count(), asset.animations.size(), static_cast<double>(rig.forward.x), static_cast<double>(rig.forward.y),
                static_cast<double>(rig.hips_height), static_cast<double>(rig.leg_length));
    for (std::size_t b = 0; b < anim::bone_count; ++b) {
        const std::int32_t j = map->joint[b];
        std::printf("  %-16s %s\n", anim::bone_name(static_cast<anim::Bone>(b)), j >= 0 ? skin.joint_names[static_cast<std::size_t>(j)].c_str() : "-");
    }
    for (const gfx::AnimationClip& clip : asset.animations) {
        bool wanted = argc == 2;
        for (int i = 2; i < argc; ++i) wanted = wanted || clip.name == argv[i];
        if (!wanted) continue;
        const anim::ClipInfo ci = anim::analyze_cycle(skin, *map, rig, clip);
        std::printf("%-22s dur %5.2f s  root %5.2f m/s  ground %5.2f m/s  stride %5.2f m  loop_err %.3f rad  hips %.3f  L[%.2f..%.2f] R[%.2f..%.2f]  %s\n", clip.name.c_str(), static_cast<double>(ci.duration),
                    static_cast<double>(ci.root_speed), static_cast<double>(ci.ground_speed), static_cast<double>(ci.stride_length), static_cast<double>(ci.loop_error), static_cast<double>(ci.hips_height),
                    static_cast<double>(ci.left.contact_start), static_cast<double>(ci.left.contact_end), static_cast<double>(ci.right.contact_start), static_cast<double>(ci.right.contact_end),
                    ci.valid ? "locomotion" : "-");
    }
    return 0;
}
