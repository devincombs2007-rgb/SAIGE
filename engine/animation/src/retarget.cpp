#include "saige/animation/retarget.hpp"

#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;
using render::Quat;
using render::Transform;

namespace {

float yaw_of(Float3 forward) { return std::atan2(forward.y, forward.x); }

} // namespace

gfx::AnimationClip retarget_clip(const RetargetSource& src, const gfx::AnimationClip& clip, const gfx::Skin& target, const HumanoidMap& tmap, const RigInfo& trig, const RetargetOptions& options) {
    if (src.skin == nullptr || src.map == nullptr || src.rig == nullptr || src.rest == nullptr) throw std::invalid_argument("retarget_clip: incomplete source");
    if (!(options.fps >= 1.0f) || !(clip.duration >= 0.0f)) throw std::invalid_argument("retarget_clip: bad fps or clip");
    const gfx::Skin& ss = *src.skin;
    const HumanoidMap& smap = *src.map;
    const gfx::ClipBinding binding = gfx::bind_clip(clip, ss);

    const std::vector<JointWorld> s_rest = joint_world(ss, *src.rest);
    const gfx::Pose t_rest_pose = gfx::rest_pose(target);
    const std::vector<JointWorld> t_rest = joint_world(target, t_rest_pose);
    // Source bone -> target joint pairs, and the target joints in an order where parents come first.
    std::vector<std::int32_t> source_of(target.joint_count(), -1);
    for (std::size_t b = 0; b < bone_count; ++b) {
        if (smap.joint[b] >= 0 && tmap.joint[b] >= 0) source_of[static_cast<std::size_t>(tmap.joint[b])] = smap.joint[b];
    }
    std::vector<std::size_t> order;
    {
        std::vector<char> placed(target.joint_count(), 0);
        while (order.size() < target.joint_count()) {
            const std::size_t before = order.size();
            for (std::size_t j = 0; j < target.joint_count(); ++j) {
                if (placed[j]) continue;
                const std::int32_t p = target.parent[j];
                if (p >= 0 && !placed[static_cast<std::size_t>(p)]) continue;
                placed[j] = 1;
                order.push_back(j);
            }
            if (order.size() == before) throw std::invalid_argument("retarget_clip: the target hierarchy has a cycle");
        }
    }
    const float yaw = yaw_of(trig.forward) - yaw_of(src.rig->forward);  // the target faces this much further around than the source
    const Quat facing = render::from_axis_angle({0, 0, 1}, yaw);
    const float scale = options.scale_translation && src.rig->leg_length > 1e-4f ? trig.leg_length / src.rig->leg_length : 1.0f;
    const std::size_t s_hips = static_cast<std::size_t>(smap[Bone::Hips]), t_hips = static_cast<std::size_t>(tmap[Bone::Hips]);

    const int steps = static_cast<int>(std::ceil(clip.duration * options.fps));
    const int n = std::max(steps, 1);
    std::vector<gfx::Pose> poses;
    poses.reserve(static_cast<std::size_t>(n) + 1);
    for (int k = 0; k <= n; ++k) {
        const float t = std::min(clip.duration, static_cast<float>(k) / options.fps);
        gfx::Pose sp = *src.rest;
        gfx::evaluate_clip(clip, binding, t, false, sp);
        const std::vector<JointWorld> sw = joint_world(ss, sp);
        gfx::Pose tp = t_rest_pose;
        std::vector<JointWorld> tw(target.joint_count());
        for (const std::size_t j : order) {
            const std::int32_t p = target.parent[j];
            const JointWorld parent = p >= 0 ? tw[static_cast<std::size_t>(p)] : JointWorld{{0, 0, 0}, {0, 0, 0, 1}};
            const render::Mat4& base = target.base[j];
            Quat parent_rot = parent.rotation;
            Float3 parent_pos = parent.position;
            if (p < 0) {  // a top joint hangs off its parent node's pose
                const auto col = [&](int c) { return render::normalize(Float3{base.at(0, c), base.at(1, c), base.at(2, c)}); };
                parent_rot = render::from_basis(col(0), col(1), col(2));
                parent_pos = {base.at(0, 3), base.at(1, 3), base.at(2, 3)};
            }
            Transform local = t_rest_pose.local[j];
            if (source_of[j] >= 0) {
                const std::size_t sj = static_cast<std::size_t>(source_of[j]);
                // The rotation the source bone made from its rest, in the model's frame, turned to the target's facing, applied on top of the target's rest orientation.
                const Quat delta = sw[sj].rotation * render::conjugate(s_rest[sj].rotation);
                const Quat desired = render::normalize(facing * delta * render::conjugate(facing) * t_rest[j].rotation);
                local.rotation = render::normalize(render::conjugate(parent_rot) * desired);
                if (j == t_hips && options.scale_translation) {
                    const Float3 moved = sw[s_hips].position - s_rest[s_hips].position;
                    const Float3 desired_pos = t_rest[j].position + render::rotate(facing, moved) * scale;
                    local.position = render::rotate(render::conjugate(parent_rot), desired_pos - parent_pos);
                } else if (j == t_hips) {
                    local.position = t_rest_pose.local[j].position;
                }
            }
            const Quat world_rot = render::normalize(parent_rot * local.rotation);
            const Float3 world_pos = parent_pos + render::rotate(parent_rot, local.position);
            tw[j] = {world_pos, world_rot};
            tp.local[j] = local;
        }
        poses.push_back(std::move(tp));
    }
    gfx::AnimationClip out;
    out.name = clip.name + ".retargeted";
    out.duration = static_cast<float>(n) / options.fps;
    for (std::size_t j = 0; j < target.joint_count(); ++j) {
        if (source_of[j] < 0) continue;  // unmapped joints keep their rest pose
        gfx::AnimationSampler rot;
        rot.components = 4;
        for (int k = 0; k <= n; ++k) {
            Quat q = poses[static_cast<std::size_t>(k)].local[j].rotation;
            if (k > 0) {
                const std::size_t o = rot.values.size() - 4;
                if (q.x * rot.values[o] + q.y * rot.values[o + 1] + q.z * rot.values[o + 2] + q.w * rot.values[o + 3] < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
            }
            rot.times.push_back(static_cast<float>(k) / options.fps);
            rot.values.insert(rot.values.end(), {q.x, q.y, q.z, q.w});
        }
        out.samplers.push_back(std::move(rot));
        out.channels.push_back({target.joint_nodes[j], gfx::AnimationPath::Rotation, static_cast<std::int32_t>(out.samplers.size() - 1)});
        if (j == t_hips) {
            gfx::AnimationSampler pos;
            pos.components = 3;
            for (int k = 0; k <= n; ++k) {
                const Float3 p = poses[static_cast<std::size_t>(k)].local[j].position;
                pos.times.push_back(static_cast<float>(k) / options.fps);
                pos.values.insert(pos.values.end(), {p.x, p.y, p.z});
            }
            out.samplers.push_back(std::move(pos));
            out.channels.push_back({target.joint_nodes[j], gfx::AnimationPath::Translation, static_cast<std::int32_t>(out.samplers.size() - 1)});
        }
    }
    return out;
}

} // namespace saige::anim
