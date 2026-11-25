// Imports BVH motion capture, extracts its repeating cycle, retargets the cycle onto a glTF humanoid and prints what the analysis measures on the result.
// Usage: saige_anim_mocap_probe TARGET.glb FILE.bvh [FILE.bvh ...]
#include "saige/animation/bvh.hpp"
#include "saige/animation/retarget.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace saige;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: saige_anim_mocap_probe TARGET.glb FILE.bvh...\n");
        return 2;
    }
    const gfx::GltfLoadResult loaded = gfx::load_gltf_file(argv[1]);
    if (!loaded.ok() || loaded.asset->skins.empty()) {
        std::fprintf(stderr, "cannot load the target rig\n");
        return 1;
    }
    const gfx::Skin& target = loaded.asset->skins[0];
    const auto tmap = anim::map_humanoid(target);
    if (!tmap) return 1;
    const anim::RigInfo trig = anim::analyze_rig(target, *tmap);
    for (int i = 2; i < argc; ++i) {
        std::ifstream in(argv[i]);
        std::stringstream ss;
        ss << in.rdbuf();
        anim::BvhOptions opt;
        opt.unit_scale = anim::cmu_unit_scale;
        const anim::BvhResult bvh = anim::parse_bvh(ss.str(), opt);
        if (!bvh.ok()) {
            std::printf("%s: parse failed: %s\n", argv[i], bvh.error.c_str());
            continue;
        }
        std::string why;
        const auto smap = anim::map_humanoid(*bvh.skin, &why);
        if (!smap) {
            std::printf("%s: not a humanoid: %s\n", argv[i], why.c_str());
            continue;
        }
        const gfx::Pose srest = gfx::rest_pose(*bvh.skin);
        const anim::RigInfo srig = anim::analyze_rig(*bvh.skin, *smap);
        const anim::LoopResult loop = anim::extract_loop(*bvh.skin, *smap, srig, *bvh.clip);
        if (!loop.clip) {
            std::printf("%-12s %.2f s, source rig forward (%.2f %.2f) leg %.2f m, hips %.2f m: no loop: %s\n", argv[i], static_cast<double>(bvh.clip->duration), static_cast<double>(srig.forward.x),
                        static_cast<double>(srig.forward.y), static_cast<double>(srig.leg_length), static_cast<double>(srig.hips_height), loop.error.c_str());
            continue;
        }
        const anim::ClipInfo src = anim::analyze_cycle(*bvh.skin, *smap, srig, *loop.clip);
        const anim::RetargetSource rs{&*bvh.skin, &*smap, &srig, &srest};
        const gfx::AnimationClip rt = anim::retarget_clip(rs, *loop.clip, target, *tmap, trig);
        const anim::ClipInfo info = anim::analyze_cycle(target, *tmap, trig, rt);
        std::printf("%-10s cycle %.2fs travel %.2fm (%.2f m/s) seam %.3f | source ground %.2f | retargeted ground %.2f m/s stride %.2f m loop_err %.3f valid=%d\n", argv[i], static_cast<double>(loop.clip->duration),
                    static_cast<double>(loop.travel), static_cast<double>(loop.travel / loop.clip->duration), static_cast<double>(loop.seam_error), static_cast<double>(src.ground_speed),
                    static_cast<double>(info.ground_speed), static_cast<double>(info.stride_length), static_cast<double>(info.loop_error), info.valid ? 1 : 0);
    }
    return 0;
}
