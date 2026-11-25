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


// Slide inside the stance of one cycle, for real clips whose toes are never exactly flat on a nominal floor. The character moves at `speed` (a pure cycle plays when the speed is that cycle's own or
// beyond the fastest), and each foot counts while the shared phase is inside the interior of that foot's refined stance window (the middle 70% of it). Returns the mean horizontal world speed of the
// planted contact points divided by `speed`.
inline double window_slide(const anim::LocomotionSet& set, const anim::LocomotionConfig& cfg, std::size_t cycle_index, float speed, float seconds = 8.0f, float warmup = 2.0f, bool lock = false, double* limited_fraction = nullptr, double* worst_shortfall = nullptr) {
    anim::FootLockState lock_state;
    anim::LocomotionController c(set, cfg);
    const anim::LocomotionCycle& cy = set.cycles()[cycle_index];
    const float dt = 1.0f / 60.0f;
    const auto fract = [](float x) { return x - std::floor(x); };
    // Is u (a cycle phase) inside the middle 70% of the circular window [start, end]?
    const auto inside = [&](float u, const anim::FootCycle& w) {
        float len = w.contact_end - w.contact_start;
        if (len < 0.0f) len += 1.0f;
        const float into = fract(u - w.contact_start);
        return into >= 0.15f * len && into <= 0.85f * len;
    };
    const render::Float3 fwd = set.rig().forward;
    float x = 0.0f;
    render::Float3 prev[2];
    bool have = false;
    double sum = 0.0;
    int count = 0;
    int locked_ticks = 0, limited_ticks = 0;
    double worst = 0.0;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        c.update(dt, {speed, true, 0.0f});
        x += speed * dt;
        gfx::Pose pose = c.pose();
        if (lock) {
            anim::lock_planted_feet(set.skin(), set.map(), set.rig(), pose, {fwd * x, {0, 0, 0, 1}, {1, 1, 1}}, {}, lock_state, dt);
            for (int f = 0; f < 2; ++f) {
                if (lock_state.weight[f] <= 1e-4f || static_cast<float>(i) * dt <= warmup) continue;
                ++locked_ticks;
                if (lock_state.limited[f]) ++limited_ticks;
                worst = std::max(worst, static_cast<double>(lock_state.shortfall[f]));
            }
        }
        const render::Float3 now[2] = {contact(set, pose, true) + fwd * x, contact(set, pose, false) + fwd * x};
        const float u = fract(c.phase() - set.reference_phase() + cy.info.left.contact_start);
        if (have && static_cast<float>(i) * dt > warmup) {
            if (inside(u, cy.info.left)) {
                sum += std::hypot(static_cast<double>(now[0].x - prev[0].x), static_cast<double>(now[0].y - prev[0].y)) / static_cast<double>(dt);
                ++count;
            }
            if (inside(u, cy.info.right)) {
                sum += std::hypot(static_cast<double>(now[1].x - prev[1].x), static_cast<double>(now[1].y - prev[1].y)) / static_cast<double>(dt);
                ++count;
            }
        }
        prev[0] = now[0];
        prev[1] = now[1];
        have = true;
    }
    if (limited_fraction != nullptr) *limited_fraction = locked_ticks > 0 ? static_cast<double>(limited_ticks) / locked_ticks : 0.0;
    if (worst_shortfall != nullptr) *worst_shortfall = worst;
    return count > 0 ? (sum / count) / static_cast<double>(speed) : 0.0;
}

} // namespace metrics
