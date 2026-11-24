// Measurements shared by the animation tests: foot sliding, planted coverage, pose steps.
#pragma once

#include "fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace metrics {

using namespace saige;
using render::Float3;

struct Slide {
    double mean = 0.0;        // m/s of horizontal world speed of a planted contact point, averaged over the planted samples
    double max = 0.0;
    int planted = 0;
    double max_step = 0.0;    // the largest rotation change (rad) of any joint between two consecutive ticks
};

inline Float3 contact(const anim::LocomotionSet& set, const gfx::Pose& pose, bool left) {
    const std::vector<anim::JointWorld> w = anim::joint_world(set.skin(), pose);
    const anim::HumanoidMap& m = set.map();
    const anim::Bone b = left ? (m.has(anim::Bone::LeftToes) ? anim::Bone::LeftToes : anim::Bone::LeftFoot) : (m.has(anim::Bone::RightToes) ? anim::Bone::RightToes : anim::Bone::RightFoot);
    return w[static_cast<std::size_t>(m[b])].position;
}

// Drives a controller kinematically (the character advances `speed * dt` along its facing each tick) and measures how fast planted feet move over the ground. A contact point counts as planted
// when it is below `plant_height` for this tick and the two before.
template <class Speed>
Slide measure_slide(const anim::LocomotionSet& set, const anim::LocomotionConfig& cfg, Speed speed_at, float seconds, float warmup = 0.6f, float plant_height = 0.003f, float dt = 1.0f / 60.0f) {
    anim::LocomotionController c(set, cfg);
    Slide out;
    const Float3 fwd = set.rig().forward;
    float x = 0.0f;
    Float3 prev[2];
    float prev_z[2] = {1.0f, 1.0f};  // the height one tick before `prev`: a contact point is planted for a tick only if it was low for the two ticks before as well, so the landing and lift ticks are not scored
    bool have_prev = false;
    gfx::Pose last = c.pose();
    double sum = 0.0;
    const int steps = static_cast<int>(seconds / dt);
    for (int i = 0; i < steps; ++i) {
        const float v = speed_at(static_cast<float>(i) * dt);
        c.update(dt, {v, true, 0.0f});
        x += v * dt;
        const Float3 now[2] = {contact(set, c.pose(), true) + fwd * x, contact(set, c.pose(), false) + fwd * x};
        if (have_prev && static_cast<float>(i) * dt > warmup) {
            for (int f = 0; f < 2; ++f) {
                const float zp = prev[f].z, zn = now[f].z;
                if (prev_z[f] < plant_height && zp < plant_height && zn < plant_height) {
                    const double s = std::hypot(static_cast<double>(now[f].x - prev[f].x), static_cast<double>(now[f].y - prev[f].y)) / static_cast<double>(dt);
                    sum += s;
                    out.max = std::max(out.max, s);
                    ++out.planted;
                }
            }
            out.max_step = std::max(out.max_step, static_cast<double>(anim::pose_distance(c.pose(), last)));
        }
        prev_z[0] = prev[0].z;
        prev_z[1] = prev[1].z;
        prev[0] = now[0];
        prev[1] = now[1];
        have_prev = true;
        last = c.pose();
    }
    out.mean = out.planted > 0 ? sum / out.planted : 0.0;
    return out;
}

} // namespace metrics
