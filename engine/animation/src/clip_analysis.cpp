#include "saige/animation/clip_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;

namespace {

float dot2(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y; }
float len2(Float3 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

Float3 horizontal_unit(Float3 v, Float3 fallback) {
    const float l = len2(v);
    return l > 1e-6f ? Float3{v.x / l, v.y / l, 0.0f} : fallback;
}

struct Planted {
    std::vector<char> flag;
    int start = -1, end = -1;  // the longest planted run: first and last sample index (circular, end may be < start)
    float fraction = 0.0f;
};

Planted find_planted(const std::vector<float>& height, int n) {
    float lo = height[0], hi = height[0];
    for (int k = 0; k < n; ++k) {
        lo = std::min(lo, height[static_cast<std::size_t>(k)]);
        hi = std::max(hi, height[static_cast<std::size_t>(k)]);
    }
    const float threshold = lo + 0.025f + 0.03f * (hi - lo);  // a few centimeters above the lowest point of the cycle
    Planted p;
    p.flag.assign(static_cast<std::size_t>(n), 0);
    for (int k = 0; k < n; ++k) p.flag[static_cast<std::size_t>(k)] = height[static_cast<std::size_t>(k)] <= threshold ? 1 : 0;
    // The longest circular run of planted samples.
    int best_len = 0;
    for (int k = 0; k < n; ++k) {
        if (!p.flag[static_cast<std::size_t>(k)] || p.flag[static_cast<std::size_t>((k + n - 1) % n)]) continue;  // not the start of a run
        int len = 0;
        while (len < n && p.flag[static_cast<std::size_t>((k + len) % n)]) ++len;
        if (len > best_len) {
            best_len = len;
            p.start = k;
            p.end = (k + len - 1) % n;
        }
    }
    p.fraction = static_cast<float>(best_len) / static_cast<float>(n);
    return p;
}

} // namespace

RigInfo analyze_rig(const gfx::Skin& skin, const HumanoidMap& map) {
    RigInfo info;
    const gfx::Pose rest = gfx::rest_pose(skin);
    const std::vector<JointWorld> w = joint_world(skin, rest);
    const auto pos = [&](Bone b) { return w[static_cast<std::size_t>(map[b])].position; };
    const Float3 lateral = pos(Bone::LeftUpperLeg) - pos(Bone::RightUpperLeg);  // points to the character's left
    info.left = horizontal_unit(lateral, {0, 1, 0});
    // forward x left = up in the engine's frame, so forward = left x up.
    info.forward = {info.left.y, -info.left.x, 0.0f};
    const float ankle_z = std::min(pos(Bone::LeftFoot).z, pos(Bone::RightFoot).z);
    float floor_z = ankle_z;  // the lowest point of the feet at rest: the floor the rig stands on
    if (map.has(Bone::LeftToes)) floor_z = std::min(floor_z, pos(Bone::LeftToes).z);
    if (map.has(Bone::RightToes)) floor_z = std::min(floor_z, pos(Bone::RightToes).z);
    const float contact_z = map.has(Bone::LeftToes) ? pos(Bone::LeftToes).z : pos(Bone::LeftFoot).z;
    info.contact_offset = std::max(0.0f, contact_z - floor_z);
    info.hips_height = pos(Bone::Hips).z - floor_z;
    const auto dist = [&](Bone a, Bone b) {
        const Float3 d = pos(a) - pos(b);
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    };
    info.leg_length = dist(Bone::LeftUpperLeg, Bone::LeftLowerLeg) + dist(Bone::LeftLowerLeg, Bone::LeftFoot) + (ankle_z - floor_z);
    return info;
}

ClipInfo analyze_cycle(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, int samples) {
    ClipInfo info;
    if (!(clip.duration > 0.0f) || samples < 8) return info;
    const gfx::ClipBinding binding = gfx::bind_clip(clip, skin);
    const int n = samples;
    std::vector<Float3> hips(static_cast<std::size_t>(n) + 1), lf(hips.size()), rf(hips.size());
    gfx::Pose first, last;
    for (int k = 0; k <= n; ++k) {
        gfx::Pose pose = gfx::rest_pose(skin);
        gfx::evaluate_clip(clip, binding, clip.duration * static_cast<float>(k) / static_cast<float>(n), false, pose);
        if (k == 0) first = pose;
        if (k == n) last = pose;
        const std::vector<JointWorld> w = joint_world(skin, pose);
        hips[static_cast<std::size_t>(k)] = w[static_cast<std::size_t>(map[Bone::Hips])].position;
        // The contact point of a foot is its toe joint (the ball of the foot) when the rig has one: an ankle drifts through the heel-to-toe roll, which would bias the speed; without toes, the ankle.
        lf[static_cast<std::size_t>(k)] = w[static_cast<std::size_t>(map.has(Bone::LeftToes) ? map[Bone::LeftToes] : map[Bone::LeftFoot])].position;
        rf[static_cast<std::size_t>(k)] = w[static_cast<std::size_t>(map.has(Bone::RightToes) ? map[Bone::RightToes] : map[Bone::RightFoot])].position;
    }
    info.duration = clip.duration;
    info.loop_error = pose_distance(first, last);
    const Float3 net = hips[static_cast<std::size_t>(n)] - hips[0];
    const Float3 drift{net.x / clip.duration, net.y / clip.duration, 0.0f};
    info.drift_velocity = drift;
    info.root_speed = dot2(drift, rig.forward);
    float mean_hips = 0.0f;
    for (int k = 0; k < n; ++k) mean_hips += hips[static_cast<std::size_t>(k)].z;
    info.hips_height = mean_hips / static_cast<float>(n);

    // Foot heights above the lowest point of the cycle, then the planted runs.
    const auto heights = [&](const std::vector<Float3>& f) {
        std::vector<float> h(static_cast<std::size_t>(n));
        float lo = f[0].z;
        for (int k = 0; k < n; ++k) lo = std::min(lo, f[static_cast<std::size_t>(k)].z);
        for (int k = 0; k < n; ++k) h[static_cast<std::size_t>(k)] = f[static_cast<std::size_t>(k)].z - lo;
        return h;
    };
    info.left_height = heights(lf);
    info.right_height = heights(rf);
    Planted pl = find_planted(info.left_height, n), pr = find_planted(info.right_height, n);
    if (pl.start < 0 || pr.start < 0) return info;  // a foot never planted: not a locomotion cycle

    // The ground speed: in the in-place version of the clip, a planted contact point moves backward at exactly the speed the character must move. Central differences over the cycle (circular),
    // contact points minus the linear drift; only the interior of the low run counts.
    const float dt = clip.duration / static_cast<float>(n);
    const auto backward_speed = [&](const std::vector<Float3>& f, int k) {
        const std::size_t prev = static_cast<std::size_t>((k + n - 1) % n), next = static_cast<std::size_t>((k + 1) % n);
        Float3 a = f[prev], b = f[next];
        if (k == 0) a = a - Float3{net.x, net.y, 0.0f};          // prev is sample n - 1, one cycle later in time
        if (k == n - 1) b = b + Float3{net.x, net.y, 0.0f};      // next is sample 0, one cycle earlier
        const Float3 v{(b.x - a.x) / (2.0f * dt) - drift.x, (b.y - a.y) / (2.0f * dt) - drift.y, 0.0f};
        return -dot2(v, rig.forward);
    };
    std::vector<float> speeds;
    const auto collect = [&](const std::vector<Float3>& f, const Planted& p) {
        for (int k = 0; k < n; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            if (!p.flag[i] || !p.flag[static_cast<std::size_t>((k + n - 1) % n)] || !p.flag[static_cast<std::size_t>((k + 1) % n)]) continue;
            speeds.push_back(backward_speed(f, k));
        }
    };
    collect(lf, pl);
    collect(rf, pr);
    if (speeds.empty()) return info;
    // The median: the first and last low samples of a run still carry the foot's landing and lift, and a median ignores them.
    const std::size_t mid = speeds.size() / 2;
    std::nth_element(speeds.begin(), speeds.begin() + static_cast<std::ptrdiff_t>(mid), speeds.end());
    info.ground_speed = speeds[mid];
    info.stride_length = info.ground_speed * clip.duration;

    // Refine the planted intervals with the speed: a foot is planted while it is low *and* moving backward at the ground speed (a foot just lifting or just landing is low but is not yet at
    // that speed), which gives the true contact timing.
    const float tolerance = std::max(0.35f * info.ground_speed, 0.15f);
    const auto refine = [&](const std::vector<Float3>& f, Planted& p) {
        for (int k = 0; k < n; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            if (p.flag[i] && std::fabs(backward_speed(f, k) - info.ground_speed) > tolerance) p.flag[i] = 0;
        }
        p.start = -1;
        int best_len = 0;
        for (int k = 0; k < n; ++k) {
            if (!p.flag[static_cast<std::size_t>(k)] || p.flag[static_cast<std::size_t>((k + n - 1) % n)]) continue;
            int len = 0;
            while (len < n && p.flag[static_cast<std::size_t>((k + len) % n)]) ++len;
            if (len > best_len) {
                best_len = len;
                p.start = k;
                p.end = (k + len - 1) % n;
            }
        }
        p.fraction = static_cast<float>(best_len) / static_cast<float>(n);
    };
    refine(lf, pl);
    refine(rf, pr);
    if (pl.start < 0 || pr.start < 0) return info;
    // With the true stance windows known, the ground speed is the mean backward speed over their interior (the middle 70%), which is what the planted foot actually does: a median over the
    // whole low run (the first pass) is robust to the landing and the lift but sits a little off when the speed varies through the stance.
    {
        double sum = 0.0;
        int count = 0;
        const auto interior = [&](const std::vector<Float3>& f, const Planted& p) {
            int len = 0;
            while (len < n && p.flag[static_cast<std::size_t>((p.start + len) % n)]) ++len;
            const int skip = static_cast<int>(std::floor(0.15f * static_cast<float>(len)));
            for (int i = skip; i < len - skip; ++i) {
                sum += static_cast<double>(backward_speed(f, (p.start + i) % n));
                ++count;
            }
        };
        interior(lf, pl);
        interior(rf, pr);
        if (count >= 4) {
            info.ground_speed = static_cast<float>(sum / count);
            info.stride_length = info.ground_speed * clip.duration;
        }
    }
    const auto cycle = [&](const Planted& p) {
        FootCycle c;
        c.contact_start = static_cast<float>(p.start) / static_cast<float>(n);
        c.contact_end = static_cast<float>(p.end) / static_cast<float>(n);
        c.planted_fraction = p.fraction;
        return c;
    };
    info.left = cycle(pl);
    info.right = cycle(pr);
    info.valid = true;
    return info;
}

void evaluate_in_place(const gfx::Skin& skin, const HumanoidMap&, const RigInfo&, const gfx::AnimationClip& clip, const gfx::ClipBinding& binding, float time, const ClipInfo& info, gfx::Pose& pose) {
    gfx::evaluate_clip(clip, binding, time, true, pose);
    if (!(clip.duration > 0.0f)) return;
    float t = std::fmod(std::isfinite(time) ? time : 0.0f, clip.duration);
    if (t < 0.0f) t += clip.duration;
    const Float3 shift{-info.drift_velocity.x * t, -info.drift_velocity.y * t, 0.0f};
    translate_model(skin, shift, pose);
}

} // namespace saige::anim
