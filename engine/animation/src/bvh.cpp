#include "saige/animation/bvh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace saige::anim {

using render::Float3;
using render::Mat4;
using render::Quat;
using render::Transform;

namespace {

constexpr float deg2rad = 3.14159265358979f / 180.0f;

enum class Channel { Xpos, Ypos, Zpos, Xrot, Yrot, Zrot };

struct BvhJoint {
    std::string name;
    int parent = -1;
    Float3 offset{};
    std::vector<Channel> channels;
    int first_value = 0;  // index of this joint's first channel value within a frame
};

struct Tokens {
    std::vector<std::string> t;
    std::size_t i = 0;
    bool done() const { return i >= t.size(); }
    const std::string& peek() const { return t[i]; }
    std::string next() {
        if (done()) throw std::runtime_error("unexpected end of file");
        return t[i++];
    }
    void expect(const char* word) {
        const std::string got = next();
        if (got != word) throw std::runtime_error(std::string("expected '") + word + "', found '" + got + "'");
    }
};

Float3 to_engine(Float3 v) { return {v.z, v.x, v.y}; }  // the glTF convention: (x, y, z) -> (z, x, y)

using M3 = std::array<std::array<float, 3>, 3>;

M3 axis_rotation(char axis, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    switch (axis) {
        case 'x': return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
        case 'y': return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
        default: return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
    }
}

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) r[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += a[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] * b[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
        }
    }
    return r;
}

// Rotation of a joint from its Euler channels, in the file's order (the first listed channel is the outermost rotation), as an engine-space quaternion: R_engine = P R P^T with P the
// permutation behind to_engine.
Quat rotation_from_channels(const BvhJoint& joint, const float* values) {
    M3 r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (std::size_t c = 0; c < joint.channels.size(); ++c) {
        const Channel ch = joint.channels[c];
        if (ch == Channel::Xrot) r = mul(r, axis_rotation('x', values[c] * deg2rad));
        else if (ch == Channel::Yrot) r = mul(r, axis_rotation('y', values[c] * deg2rad));
        else if (ch == Channel::Zrot) r = mul(r, axis_rotation('z', values[c] * deg2rad));
    }
    // P maps (x, y, z) to (z, x, y): engine row 0 = file z, row 1 = file x, row 2 = file y.
    constexpr int p[3] = {2, 0, 1};
    M3 e{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) e[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = r[static_cast<std::size_t>(p[i])][static_cast<std::size_t>(p[j])];
    }
    const auto col = [&](std::size_t c) { return Float3{e[0][c], e[1][c], e[2][c]}; };
    return render::from_basis(col(0), col(1), col(2));
}

Float3 translation_from_channels(const BvhJoint& joint, const float* values, float scale) {
    Float3 v{};
    for (std::size_t c = 0; c < joint.channels.size(); ++c) {
        if (joint.channels[c] == Channel::Xpos) v.x = values[c];
        else if (joint.channels[c] == Channel::Ypos) v.y = values[c];
        else if (joint.channels[c] == Channel::Zpos) v.z = values[c];
    }
    return to_engine(v) * scale;
}

bool has_rotation(const BvhJoint& j) {
    for (const Channel c : j.channels) {
        if (c == Channel::Xrot || c == Channel::Yrot || c == Channel::Zrot) return true;
    }
    return false;
}
bool has_translation(const BvhJoint& j) {
    for (const Channel c : j.channels) {
        if (c == Channel::Xpos || c == Channel::Ypos || c == Channel::Zpos) return true;
    }
    return false;
}

void parse_joint(Tokens& tk, std::vector<BvhJoint>& joints, int parent, std::size_t max_joints, int& value_count, bool is_root) {
    BvhJoint j;
    j.name = tk.next();
    j.parent = parent;
    tk.expect("{");
    const int index = static_cast<int>(joints.size());
    if (joints.size() >= max_joints) throw std::runtime_error("too many joints");
    joints.push_back(j);
    while (true) {
        const std::string w = tk.next();
        if (w == "OFFSET") {
            Float3 o{};
            o.x = std::strtof(tk.next().c_str(), nullptr);
            o.y = std::strtof(tk.next().c_str(), nullptr);
            o.z = std::strtof(tk.next().c_str(), nullptr);
            joints[static_cast<std::size_t>(index)].offset = o;
        } else if (w == "CHANNELS") {
            const int n = std::atoi(tk.next().c_str());
            if (n < 0 || n > 6) throw std::runtime_error("bad channel count");
            BvhJoint& jj = joints[static_cast<std::size_t>(index)];
            jj.first_value = value_count;
            for (int c = 0; c < n; ++c) {
                const std::string name = tk.next();
                if (name == "Xposition") jj.channels.push_back(Channel::Xpos);
                else if (name == "Yposition") jj.channels.push_back(Channel::Ypos);
                else if (name == "Zposition") jj.channels.push_back(Channel::Zpos);
                else if (name == "Xrotation") jj.channels.push_back(Channel::Xrot);
                else if (name == "Yrotation") jj.channels.push_back(Channel::Yrot);
                else if (name == "Zrotation") jj.channels.push_back(Channel::Zrot);
                else throw std::runtime_error("unknown channel " + name);
            }
            value_count += n;
        } else if (w == "JOINT") {
            parse_joint(tk, joints, index, max_joints, value_count, false);
        } else if (w == "End") {
            tk.expect("Site");
            tk.expect("{");
            int depth = 1;
            while (depth > 0) {
                const std::string x = tk.next();
                if (x == "{") ++depth;
                else if (x == "}") --depth;
            }
        } else if (w == "}") {
            break;
        } else {
            throw std::runtime_error("unexpected '" + w + "' in joint " + joints[static_cast<std::size_t>(index)].name);
        }
    }
    (void)is_root;
}

Mat4 rigid_inverse(const Mat4& m) {
    Mat4 r = Mat4::identity();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.at(i, j) = m.at(j, i);
    }
    const Float3 p{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    for (int i = 0; i < 3; ++i) r.at(i, 3) = -(r.at(i, 0) * p.x + r.at(i, 1) * p.y + r.at(i, 2) * p.z);
    return r;
}

} // namespace

BvhResult parse_bvh(std::string_view text, const BvhOptions& options) {
    BvhResult out;
    try {
        Tokens tk;
        {
            std::istringstream in{std::string(text)};
            std::string w;
            while (in >> w) tk.t.push_back(w);
        }
        tk.expect("HIERARCHY");
        std::vector<BvhJoint> joints;
        int value_count = 0;
        tk.expect("ROOT");
        parse_joint(tk, joints, -1, options.max_joints, value_count, true);
        tk.expect("MOTION");
        tk.expect("Frames:");
        const long frames = std::atol(tk.next().c_str());
        tk.expect("Frame");
        tk.expect("Time:");
        const float dt = std::strtof(tk.next().c_str(), nullptr);
        if (frames < 2 || static_cast<std::size_t>(frames) > options.max_frames || !(dt > 0.0f) || !std::isfinite(dt)) throw std::runtime_error("bad frame count or frame time");
        std::vector<float> values(static_cast<std::size_t>(frames) * static_cast<std::size_t>(value_count));
        for (float& v : values) {
            if (tk.done()) throw std::runtime_error("the motion data is shorter than the header says");
            v = std::strtof(tk.next().c_str(), nullptr);
            if (!std::isfinite(v)) throw std::runtime_error("non-finite motion value");
        }
        const std::size_t first = options.first_frame_is_rest ? 1 : 0;
        if (static_cast<std::size_t>(frames) < first + 2) throw std::runtime_error("too few frames");
        const auto frame = [&](std::size_t f) { return values.data() + f * static_cast<std::size_t>(value_count); };

        gfx::Skin skin;
        const std::size_t n = joints.size();
        for (std::size_t i = 0; i < n; ++i) {
            skin.joint_nodes.push_back(static_cast<std::int32_t>(i));
            skin.joint_names.push_back(joints[i].name);
            skin.parent.push_back(joints[i].parent);
            skin.base.push_back(Mat4::identity());
            Transform rest;
            rest.position = to_engine(joints[i].offset) * options.unit_scale;
            rest.rotation = {0, 0, 0, 1};
            if (options.first_frame_is_rest) {
                if (has_rotation(joints[i])) rest.rotation = rotation_from_channels(joints[i], frame(0) + joints[i].first_value);
                if (has_translation(joints[i])) rest.position = rest.position + translation_from_channels(joints[i], frame(0) + joints[i].first_value, options.unit_scale);
            }
            skin.rest.push_back(rest);
        }
        std::vector<Mat4> global(n, Mat4::identity());
        for (std::size_t i = 0; i < n; ++i) {  // parents precede children in a BVH
            const Mat4 local = render::to_matrix(skin.rest[i]);
            global[i] = joints[i].parent >= 0 ? global[static_cast<std::size_t>(joints[i].parent)] * local : local;
            skin.inverse_bind.push_back(rigid_inverse(global[i]));
        }

        gfx::AnimationClip clip;
        clip.name = "bvh";
        const std::size_t count = static_cast<std::size_t>(frames) - first;
        for (std::size_t i = 0; i < n; ++i) {
            const BvhJoint& j = joints[i];
            if (has_rotation(j)) {
                gfx::AnimationSampler s;
                s.components = 4;
                s.interpolation = gfx::Interpolation::Linear;
                Quat prev{0, 0, 0, 1};
                for (std::size_t f = 0; f < count; ++f) {
                    Quat q = rotation_from_channels(j, frame(first + f) + j.first_value);
                    if (f > 0 && q.x * prev.x + q.y * prev.y + q.z * prev.z + q.w * prev.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};  // keep the path short
                    prev = q;
                    s.times.push_back(static_cast<float>(f) * dt);
                    s.values.insert(s.values.end(), {q.x, q.y, q.z, q.w});
                }
                clip.samplers.push_back(std::move(s));
                clip.channels.push_back({static_cast<std::int32_t>(i), gfx::AnimationPath::Rotation, static_cast<std::int32_t>(clip.samplers.size() - 1)});
            }
            if (has_translation(j)) {
                gfx::AnimationSampler s;
                s.components = 3;
                s.interpolation = gfx::Interpolation::Linear;
                for (std::size_t f = 0; f < count; ++f) {
                    const Float3 t = translation_from_channels(j, frame(first + f) + j.first_value, options.unit_scale);
                    s.times.push_back(static_cast<float>(f) * dt);
                    s.values.insert(s.values.end(), {t.x, t.y, t.z});
                }
                clip.samplers.push_back(std::move(s));
                clip.channels.push_back({static_cast<std::int32_t>(i), gfx::AnimationPath::Translation, static_cast<std::int32_t>(clip.samplers.size() - 1)});
            }
        }
        clip.duration = static_cast<float>(count - 1) * dt;
        out.skin = std::move(skin);
        out.clip = std::move(clip);
        out.frame_time = dt;
    } catch (const std::exception& e) {
        out.error = e.what();
        out.skin.reset();
        out.clip.reset();
    }
    return out;
}

// ---- loop extraction ---------------------------------------------------------------------------------------------------------------------------------------------

namespace {

Quat yaw_quat(float radians) { return render::from_axis_angle({0, 0, 1}, radians); }

Quat base_rotation(const Mat4& m) {
    const auto col = [&](int c) { return render::normalize(Float3{m.at(0, c), m.at(1, c), m.at(2, c)}); };
    return render::from_basis(col(0), col(1), col(2));
}

// Rotates the whole model about the vertical by `radians`: every top joint's rotation and translation, through its base.
void yaw_top_joints(const gfx::Skin& skin, gfx::Pose& pose, float radians) {
    const Quat rz = yaw_quat(radians);
    for (std::size_t j = 0; j < skin.joint_count(); ++j) {
        if (skin.parent[j] >= 0) continue;
        const Quat b = base_rotation(skin.base[j]);
        const Quat in_local = render::conjugate(b) * rz * b;  // the model-space yaw seen from the joint's parent space
        pose.local[j].position = render::rotate(in_local, pose.local[j].position);
        pose.local[j].rotation = render::normalize(in_local * pose.local[j].rotation);
    }
}

void shift_top_joints(const gfx::Skin& skin, gfx::Pose& pose, Float3 model_shift) {
    for (std::size_t j = 0; j < skin.joint_count(); ++j) {
        if (skin.parent[j] >= 0) continue;
        const Mat4& b = skin.base[j];
        const Float3 local{b.at(0, 0) * model_shift.x + b.at(1, 0) * model_shift.y + b.at(2, 0) * model_shift.z, b.at(0, 1) * model_shift.x + b.at(1, 1) * model_shift.y + b.at(2, 1) * model_shift.z,
                           b.at(0, 2) * model_shift.x + b.at(1, 2) * model_shift.y + b.at(2, 2) * model_shift.z};
        pose.local[j].position = pose.local[j].position + local;
    }
}

float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

} // namespace

LoopResult extract_loop(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, const LoopOptions& options, float fps) {
    LoopResult result;
    if (!(fps >= 10.0f) || !(clip.duration > 0.0f)) {
        result.error = "bad fps or an empty clip";
        return result;
    }
    const gfx::ClipBinding binding = gfx::bind_clip(clip, skin);
    const int frames = static_cast<int>(std::floor(clip.duration * fps)) + 1;
    const int min_len = static_cast<int>(std::ceil(options.min_duration * fps)), max_len = static_cast<int>(std::floor(options.max_duration * fps));
    const int margin = static_cast<int>(std::ceil(options.blend_window * fps));
    if (frames < min_len + margin + 2) {
        result.error = "the clip is shorter than one cycle";
        return result;
    }
    const gfx::Pose rest = gfx::rest_pose(skin);
    const std::vector<JointWorld> rest_world = joint_world(skin, rest);
    const Quat hips_rest = rest_world[static_cast<std::size_t>(map[Bone::Hips])].rotation;
    const std::array<Bone, 10> tracked = {Bone::Spine, Bone::Chest, Bone::Head, Bone::LeftLowerArm, Bone::RightLowerArm, Bone::LeftLowerLeg, Bone::RightLowerLeg, Bone::LeftFoot, Bone::RightFoot, Bone::Hips};

    struct Frame {
        std::array<Float3, 10> p;  // heading-relative positions of the tracked joints
        Float3 hips{};
        float yaw = 0.0f;
    };
    std::vector<Frame> f(static_cast<std::size_t>(frames));
    for (int k = 0; k < frames; ++k) {
        gfx::Pose pose = rest;
        gfx::evaluate_clip(clip, binding, static_cast<float>(k) / fps, false, pose);
        const std::vector<JointWorld> w = joint_world(skin, pose);
        const JointWorld& h = w[static_cast<std::size_t>(map[Bone::Hips])];
        const Float3 fwd = render::rotate(h.rotation * render::conjugate(hips_rest), rig.forward);
        Frame& fr = f[static_cast<std::size_t>(k)];
        fr.yaw = std::atan2(fwd.y, fwd.x);
        fr.hips = h.position;
        const Quat unyaw = yaw_quat(-fr.yaw);
        for (std::size_t b = 0; b < tracked.size(); ++b) {
            const Float3 d = w[static_cast<std::size_t>(map[tracked[b]])].position - Float3{h.position.x, h.position.y, 0.0f};
            fr.p[b] = render::rotate(unyaw, d);
        }
    }
    // A pose-and-velocity distance: positions at k and the change over three frames, so a foot swinging forward does not match one swinging back.
    const auto distance = [&](int a, int b) {
        double sum = 0.0;
        for (std::size_t t = 0; t < tracked.size(); ++t) {
            const Float3 pa = f[static_cast<std::size_t>(a)].p[t], pb = f[static_cast<std::size_t>(b)].p[t];
            const Float3 va = f[static_cast<std::size_t>(a + 3)].p[t] - pa, vb = f[static_cast<std::size_t>(b + 3)].p[t] - pb;
            const Float3 dp = pa - pb, dv = va - vb;
            sum += static_cast<double>(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z) + 4.0 * static_cast<double>(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
        }
        return sum;
    };
    double best = 1e30;
    int best_a = -1, best_b = -1;
    const float max_drift = options.max_heading_drift_deg * deg2rad;
    for (int a = margin; a + min_len + 3 < frames; ++a) {
        for (int len = min_len; len <= max_len && a + len + 3 < frames; ++len) {
            const int b = a + len;
            const float drift = wrap_pi(f[static_cast<std::size_t>(b)].yaw - f[static_cast<std::size_t>(a)].yaw);
            if (std::fabs(drift) > max_drift) continue;
            const double d = distance(a, b);
            if (d < best) {
                best = d;
                best_a = a;
                best_b = b;
            }
        }
    }
    if (best_a < 0) {
        result.error = "no repeating stretch within the heading limit";
        return result;
    }

    const int n = best_b - best_a;
    const float ta = static_cast<float>(best_a) / fps, period = static_cast<float>(n) / fps;
    const float heading = f[static_cast<std::size_t>(best_a)].yaw;
    const float drift_yaw = wrap_pi(f[static_cast<std::size_t>(best_b)].yaw - f[static_cast<std::size_t>(best_a)].yaw);
    // The net travel over the stretch, with the whole model turned to face +X (the heading at the start).
    const Float3 raw_travel = f[static_cast<std::size_t>(best_b)].hips - f[static_cast<std::size_t>(best_a)].hips;
    const Float3 travel = render::rotate(yaw_quat(-heading), Float3{raw_travel.x, raw_travel.y, 0.0f});
    result.travel = travel.x * rig.forward.x + travel.y * rig.forward.y;
    result.source_start = ta;
    result.source_end = ta + period;
    {
        gfx::Pose pa = rest, pb = rest;
        gfx::evaluate_clip(clip, binding, ta, false, pa);
        gfx::evaluate_clip(clip, binding, ta + period, false, pb);
        result.seam_error = pose_distance(pa, pb);
    }
    // One stripped sample of the source at time ts: facing +X, heading drift and linear travel removed.
    const auto stripped = [&](float ts) {
        gfx::Pose pose = rest;
        gfx::evaluate_clip(clip, binding, ts, false, pose);
        yaw_top_joints(skin, pose, -heading);
        const float u = (ts - ta) / period;
        if (std::fabs(drift_yaw) > 0.0f) yaw_top_joints(skin, pose, -drift_yaw * u);
        // The remaining travel is along the rotated direction; strip it at the same fraction.
        shift_top_joints(skin, pose, Float3{-travel.x * u, -travel.y * u, 0.0f});
        return pose;
    };

    gfx::AnimationClip out;
    out.name = clip.name + ".loop";
    const int window = std::max(2, margin);
    std::vector<gfx::Pose> poses;
    poses.reserve(static_cast<std::size_t>(n) + 1);
    for (int k = 0; k <= n; ++k) {
        const float ts = ta + static_cast<float>(k) / fps;
        gfx::Pose pose = stripped(ts);
        const int into = k - (n - window);
        if (into > 0) {
            // Close the seam: the last frames fade toward the stretch one cycle earlier, which ends where the loop begins.
            const float w = std::min(1.0f, static_cast<float>(into) / static_cast<float>(window));
            const float s = w * w * (3.0f - 2.0f * w);
            blend_into(pose, stripped(ts - period), s);
        }
        poses.push_back(std::move(pose));
    }
    out.duration = period;
    const std::size_t joints = skin.joint_count();
    for (std::size_t j = 0; j < joints; ++j) {
        gfx::AnimationSampler rot;
        rot.components = 4;
        for (int k = 0; k <= n; ++k) {
            Quat q = poses[static_cast<std::size_t>(k)].local[j].rotation;
            if (k > 0 && !rot.values.empty()) {
                const std::size_t o = rot.values.size() - 4;
                if (q.x * rot.values[o] + q.y * rot.values[o + 1] + q.z * rot.values[o + 2] + q.w * rot.values[o + 3] < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
            }
            rot.times.push_back(static_cast<float>(k) / fps);
            rot.values.insert(rot.values.end(), {q.x, q.y, q.z, q.w});
        }
        out.samplers.push_back(std::move(rot));
        out.channels.push_back({skin.joint_nodes[j], gfx::AnimationPath::Rotation, static_cast<std::int32_t>(out.samplers.size() - 1)});
        bool translated = skin.parent[j] < 0;
        for (std::size_t c = 0; c < clip.channels.size() && !translated; ++c) {
            translated = clip.channels[c].path == gfx::AnimationPath::Translation && binding.channel_joint[c] == static_cast<std::int32_t>(j);
        }
        if (translated) {
            gfx::AnimationSampler pos;
            pos.components = 3;
            for (int k = 0; k <= n; ++k) {
                const Float3 p = poses[static_cast<std::size_t>(k)].local[j].position;
                pos.times.push_back(static_cast<float>(k) / fps);
                pos.values.insert(pos.values.end(), {p.x, p.y, p.z});
            }
            out.samplers.push_back(std::move(pos));
            out.channels.push_back({skin.joint_nodes[j], gfx::AnimationPath::Translation, static_cast<std::int32_t>(out.samplers.size() - 1)});
        }
    }
    result.clip = std::move(out);
    return result;
}

} // namespace saige::anim
