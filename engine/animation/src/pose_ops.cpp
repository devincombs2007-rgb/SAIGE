#include "saige/animation/pose_ops.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;
using render::Mat4;
using render::Quat;

namespace {

JointWorld from_matrix(const Mat4& m) {
    const auto column = [&](int c) {
        const Float3 v{m.at(0, c), m.at(1, c), m.at(2, c)};
        return render::normalize(v);
    };
    JointWorld w;
    w.position = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    w.rotation = render::from_basis(column(0), column(1), column(2));
    return w;
}

} // namespace

std::vector<JointWorld> joint_world(const gfx::Skin& skin, const gfx::Pose& pose) {
    const std::size_t n = skin.joint_count();
    if (pose.local.size() != n) throw std::invalid_argument("joint_world: the pose does not match the skin");
    std::vector<Mat4> matrices(n, Mat4::identity());
    std::vector<char> done(n, 0);
    // Parents are not guaranteed to precede their children in a skin's joint order: resolve iteratively (a skin is a forest, so n passes suffice).
    std::size_t remaining = n;
    for (std::size_t pass = 0; pass < n + 1 && remaining > 0; ++pass) {
        for (std::size_t j = 0; j < n; ++j) {
            if (done[j]) continue;
            const std::int32_t p = skin.parent[j];
            if (p >= 0 && !done[static_cast<std::size_t>(p)]) continue;
            const Mat4 parent = p >= 0 ? matrices[static_cast<std::size_t>(p)] : skin.base[j];
            matrices[j] = parent * render::to_matrix(pose.local[j]);
            done[j] = 1;
            --remaining;
        }
    }
    if (remaining > 0) throw std::invalid_argument("joint_world: the skin's joint hierarchy has a cycle");
    std::vector<JointWorld> out(n);
    for (std::size_t j = 0; j < n; ++j) out[j] = from_matrix(matrices[j]);
    return out;
}

JointWorld parent_world(const gfx::Skin& skin, const std::vector<JointWorld>& world, std::size_t joint) {
    const std::int32_t p = skin.parent[joint];
    if (p < 0) return {{0, 0, 0}, {0, 0, 0, 1}};
    return world[static_cast<std::size_t>(p)];
}

void blend_into(gfx::Pose& dst, const gfx::Pose& src, float t) {
    if (dst.local.size() != src.local.size()) throw std::invalid_argument("blend_into: the poses have different joint counts");
    t = std::clamp(t, 0.0f, 1.0f);
    if (!(t > 0.0f)) return;  // also NaN
    for (std::size_t j = 0; j < dst.local.size(); ++j) {
        render::Transform& a = dst.local[j];
        const render::Transform& b = src.local[j];
        a.position = a.position + (b.position - a.position) * t;
        a.scale = a.scale + (b.scale - a.scale) * t;
        a.rotation = render::slerp(a.rotation, b.rotation, t);
    }
}

gfx::Pose blend_poses(const gfx::Pose& a, const gfx::Pose& b, float t) {
    gfx::Pose out = a;
    blend_into(out, b, t);
    return out;
}

float rotation_angle_between(Quat a, Quat b) {
    // The same rotation (identical quaternions, or one the negative of the other) is exactly 0 apart: neither acos of a dot product that rounds to 0.99999994 nor the rounding in a quaternion
    // product gives that. For the rest, the relative rotation conj(a) * b through atan2 keeps small angles accurate.
    if (a == b || a == Quat{-b.x, -b.y, -b.z, -b.w}) return 0.0f;
    const Quat r = render::conjugate(a) * b;
    const float v = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return 2.0f * std::atan2(v, std::fabs(r.w));
}

float pose_distance(const gfx::Pose& a, const gfx::Pose& b, float* max_translation) {
    if (a.local.size() != b.local.size()) throw std::invalid_argument("pose_distance: the poses have different joint counts");
    float worst = 0.0f, worst_t = 0.0f;
    for (std::size_t j = 0; j < a.local.size(); ++j) {
        worst = std::max(worst, rotation_angle_between(a.local[j].rotation, b.local[j].rotation));
        const Float3 d = a.local[j].position - b.local[j].position;
        worst_t = std::max(worst_t, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
    }
    if (max_translation != nullptr) *max_translation = worst_t;
    return worst;
}

} // namespace saige::anim

namespace saige::anim {

void translate_model(const gfx::Skin& skin, render::Float3 shift, gfx::Pose& pose) {
    for (std::size_t j = 0; j < skin.joint_count(); ++j) {
        if (skin.parent[j] >= 0) continue;
        const render::Mat4& b = skin.base[j];
        const render::Float3 local{b.at(0, 0) * shift.x + b.at(1, 0) * shift.y + b.at(2, 0) * shift.z, b.at(0, 1) * shift.x + b.at(1, 1) * shift.y + b.at(2, 1) * shift.z,
                                   b.at(0, 2) * shift.x + b.at(1, 2) * shift.y + b.at(2, 2) * shift.z};
        pose.local[j].position = pose.local[j].position + local;
    }
}

} // namespace saige::anim
