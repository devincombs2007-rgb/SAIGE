#include "saige/gfx_assets/animation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige::gfx {

using render::Float3;
using render::Float4;
using render::Mat4;
using render::Quat;
using render::Transform;

Pose rest_pose(const Skin& skin)
{
    return {skin.rest};
}

ClipBinding bind_clip(const AnimationClip& clip, const Skin& skin)
{
    ClipBinding binding;
    binding.channel_joint.assign(clip.channels.size(), -1);
    for (std::size_t c = 0; c < clip.channels.size(); ++c) {
        for (std::size_t j = 0; j < skin.joint_nodes.size(); ++j) {
            if (skin.joint_nodes[j] == clip.channels[c].node) {
                binding.channel_joint[c] = static_cast<int32_t>(j);
                break;
            }
        }
    }
    return binding;
}

namespace {

Float4 lerp4(const float* a, const float* b, float t, int n)
{
    float out[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return {out[0], out[1], out[2], out[3]};
}

Float4 load(const float* p, int n)
{
    float v[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; ++i) v[i] = p[i];
    return {v[0], v[1], v[2], v[3]};
}

} // namespace

Float4 sample_track(const AnimationSampler& s, float time)
{
    const int n = s.components;
    const std::size_t keys = s.times.size();
    if (keys == 0 || (n != 3 && n != 4)) return {0, 0, 0, n == 4 ? 1.0f : 0.0f};
    const bool cubic = s.interpolation == Interpolation::CubicSpline;
    const std::size_t stride = static_cast<std::size_t>(n) * (cubic ? 3 : 1);
    const auto value_at = [&](std::size_t key) { return s.values.data() + key * stride + (cubic ? static_cast<std::size_t>(n) : 0); };
    const auto finish = [&](Float4 v) {
        if (n == 4) {
            const Quat q = render::normalize(Quat{v.x, v.y, v.z, v.w});
            return Float4{q.x, q.y, q.z, q.w};
        }
        return v;
    };
    if (!std::isfinite(time) || time <= s.times.front()) return finish(load(value_at(0), n));
    if (time >= s.times.back()) return finish(load(value_at(keys - 1), n));
    // The segment [k, k + 1] with times[k] <= time < times[k + 1].
    const std::size_t k = static_cast<std::size_t>(std::upper_bound(s.times.begin(), s.times.end(), time) - s.times.begin()) - 1;
    if (s.interpolation == Interpolation::Step) return finish(load(value_at(k), n));
    const float dt = s.times[k + 1] - s.times[k];
    const float t = (time - s.times[k]) / dt;
    if (!cubic) {
        if (n == 4) {
            const float* a = value_at(k);
            const float* b = value_at(k + 1);
            const Quat q = render::slerp(Quat{a[0], a[1], a[2], a[3]}, Quat{b[0], b[1], b[2], b[3]}, t);
            return {q.x, q.y, q.z, q.w};
        }
        return lerp4(value_at(k), value_at(k + 1), t, n);
    }
    // glTF cubic spline: p(t) = (2t^3 - 3t^2 + 1) v_k + (t^3 - 2t^2 + t) dt b_k + (-2t^3 + 3t^2) v_{k+1} + (t^3 - t^2) dt a_{k+1}, with b_k the
    // out-tangent of key k and a_{k+1} the in-tangent of key k + 1.
    const float t2 = t * t, t3 = t2 * t;
    const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    const float* v0 = value_at(k);
    const float* b0 = v0 + n;                                  // out-tangent of key k
    const float* v1 = value_at(k + 1);
    const float* a1 = v1 - n;                                  // in-tangent of key k + 1
    float out[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; ++i) out[i] = h00 * v0[i] + h10 * dt * b0[i] + h01 * v1[i] + h11 * dt * a1[i];
    return finish({out[0], out[1], out[2], out[3]});
}

void evaluate_clip(const AnimationClip& clip, const ClipBinding& binding, float time, bool loop, Pose& pose)
{
    if (binding.channel_joint.size() != clip.channels.size()) throw std::invalid_argument("evaluate_clip: the binding does not belong to this clip");
    float t = std::isfinite(time) ? time : 0.0f;
    if (loop && clip.duration > 0.0f) {
        t = std::fmod(t, clip.duration);
        if (t < 0.0f) t += clip.duration;
    } else if (t < 0.0f) {
        t = 0.0f;
    }
    for (std::size_t c = 0; c < clip.channels.size(); ++c) {
        const int32_t joint = binding.channel_joint[c];
        if (joint < 0) continue;
        if (static_cast<std::size_t>(joint) >= pose.local.size()) throw std::invalid_argument("evaluate_clip: the pose has fewer joints than the binding");
        const AnimationChannel& channel = clip.channels[c];
        if (channel.sampler < 0 || static_cast<std::size_t>(channel.sampler) >= clip.samplers.size()) continue;
        const AnimationSampler& sampler = clip.samplers[static_cast<std::size_t>(channel.sampler)];
        if (sampler.times.empty()) continue;
        const Float4 v = sample_track(sampler, t);
        Transform& local = pose.local[static_cast<std::size_t>(joint)];
        switch (channel.path) {
        case AnimationPath::Translation: local.position = {v.x, v.y, v.z}; break;
        case AnimationPath::Rotation: local.rotation = {v.x, v.y, v.z, v.w}; break;
        case AnimationPath::Scale: local.scale = {v.x, v.y, v.z}; break;
        }
    }
}

std::vector<Mat4> skin_matrices(const Skin& skin, const Pose& pose)
{
    const std::size_t n = skin.joint_nodes.size();
    if (pose.local.size() != n || skin.parent.size() != n || skin.base.size() != n || skin.inverse_bind.size() != n) {
        throw std::invalid_argument("skin_matrices: the pose does not match the skin");
    }
    std::vector<Mat4> world(n, Mat4::identity());
    std::vector<uint8_t> done(n, 0);
    std::vector<std::size_t> chain;
    for (std::size_t start = 0; start < n; ++start) {
        // Walk up to the first joint whose world transform is known, then come back down: parents need not precede their children.
        chain.clear();
        for (std::size_t j = start; !done[j];) {
            chain.push_back(j);
            const int32_t p = skin.parent[j];
            if (p < 0) break;
            if (static_cast<std::size_t>(p) >= n || chain.size() > n) throw std::invalid_argument("skin_matrices: the skin's joint hierarchy is invalid");
            j = static_cast<std::size_t>(p);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const std::size_t j = *it;
            const int32_t p = skin.parent[j];
            const Mat4& above = p < 0 ? skin.base[j] : world[static_cast<std::size_t>(p)];
            world[j] = above * render::to_matrix(pose.local[j]);
            done[j] = 1;
        }
    }
    std::vector<Mat4> out(n);
    for (std::size_t j = 0; j < n; ++j) out[j] = world[j] * skin.inverse_bind[j];
    return out;
}

Float3 skin_position(const Primitive& primitive, std::size_t vertex, std::span<const Mat4> matrices)
{
    const Float3 p{primitive.positions[vertex * 3], primitive.positions[vertex * 3 + 1], primitive.positions[vertex * 3 + 2]};
    if (primitive.joints.size() != primitive.positions.size() / 3 * 4 || primitive.weights.size() != primitive.joints.size()) return p;
    Float3 out{0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i) {
        const float w = primitive.weights[vertex * 4 + i];
        if (w == 0.0f) continue;
        const std::size_t joint = primitive.joints[vertex * 4 + i];
        if (joint >= matrices.size()) throw std::invalid_argument("skin_position: a joint index is outside the skin matrices");
        const Float3 q = render::transform_point(matrices[joint], p);
        out = {out.x + q.x * w, out.y + q.y * w, out.z + q.z * w};
    }
    return out;
}

SkinnedVertex skin_vertex(const Primitive& primitive, std::size_t vertex, std::span<const Mat4> matrices)
{
    const auto vec3 = [&](const std::vector<float>& a) { return Float3{a[vertex * 3], a[vertex * 3 + 1], a[vertex * 3 + 2]}; };
    const Float3 p = vec3(primitive.positions);
    const Float3 n = primitive.normals.size() == primitive.positions.size() ? vec3(primitive.normals) : Float3{0, 0, 1};
    Float3 t{0, 0, 0}, b{0, 0, 0};
    if (primitive.tangents.size() == primitive.positions.size() / 3 * 4) {
        t = {primitive.tangents[vertex * 4], primitive.tangents[vertex * 4 + 1], primitive.tangents[vertex * 4 + 2]};
        b = render::cross(n, t) * primitive.tangents[vertex * 4 + 3];
    }
    if (primitive.joints.size() != primitive.positions.size() / 3 * 4 || primitive.weights.size() != primitive.joints.size()) return {p, n, t, b};
    // The blended affine matrix, column by column: B = sum_i w_i M_i.
    float blend[3][4] = {};
    for (std::size_t i = 0; i < 4; ++i) {
        const float w = primitive.weights[vertex * 4 + i];
        if (w == 0.0f) continue;
        const std::size_t joint = primitive.joints[vertex * 4 + i];
        if (joint >= matrices.size()) throw std::invalid_argument("skin_vertex: a joint index is outside the skin matrices");
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) blend[r][c] += w * matrices[joint].at(r, c);
        }
    }
    const auto apply = [&](Float3 v, float w) {
        return Float3{blend[0][0] * v.x + blend[0][1] * v.y + blend[0][2] * v.z + blend[0][3] * w,
                      blend[1][0] * v.x + blend[1][1] * v.y + blend[1][2] * v.z + blend[1][3] * w,
                      blend[2][0] * v.x + blend[2][1] * v.y + blend[2][2] * v.z + blend[2][3] * w};
    };
    const Float3 c0{blend[0][0], blend[1][0], blend[2][0]}, c1{blend[0][1], blend[1][1], blend[2][1]}, c2{blend[0][2], blend[1][2], blend[2][2]};
    SkinnedVertex out;
    out.position = apply(p, 1.0f);
    out.normal = render::cross(c1, c2) * n.x + render::cross(c2, c0) * n.y + render::cross(c0, c1) * n.z;
    out.tangent = apply(t, 0.0f);
    out.bitangent = apply(b, 0.0f);
    return out;
}

std::vector<float> skin_positions(const Primitive& primitive, std::span<const Mat4> matrices)
{
    std::vector<float> out(primitive.positions.size());
    for (std::size_t v = 0; v < primitive.positions.size() / 3; ++v) {
        const Float3 p = skin_position(primitive, v, matrices);
        out[v * 3] = p.x;
        out[v * 3 + 1] = p.y;
        out[v * 3 + 2] = p.z;
    }
    return out;
}

} // namespace saige::gfx
