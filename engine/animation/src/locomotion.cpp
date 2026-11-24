#include "saige/animation/locomotion.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

namespace {

float smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float fract(float x) { return x - std::floor(x); }

} // namespace

LocomotionSet::LocomotionSet(const gfx::Skin& skin, const HumanoidMap& map) : skin_(&skin), map_(map), rig_(analyze_rig(skin, map)) {}

bool LocomotionSet::add_cycle(const gfx::AnimationClip& clip, std::string* why, float max_loop_error) {
    const auto fail = [&](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    LocomotionCycle c;
    c.clip = clip;
    c.binding = gfx::bind_clip(c.clip, *skin_);
    c.info = analyze_cycle(*skin_, map_, rig_, c.clip);
    if (!c.info.valid) return fail("no planted feet or no measurable speed");
    if (!(c.info.ground_speed > 0.2f)) return fail("ground speed below 0.2 m/s: not a locomotion cycle");
    if (c.info.loop_error > max_loop_error) return fail("the clip does not loop (first and last poses differ)");
    // Keep ascending order; of two cycles at nearly the same speed keep the better loop.
    for (auto it = cycles_.begin(); it != cycles_.end(); ++it) {
        if (std::fabs(it->info.ground_speed - c.info.ground_speed) < 0.15f) {
            if (c.info.loop_error < it->info.loop_error) *it = std::move(c);
            return true;
        }
    }
    const auto at = std::find_if(cycles_.begin(), cycles_.end(), [&](const LocomotionCycle& o) { return o.info.ground_speed > c.info.ground_speed; });
    cycles_.insert(at, std::move(c));
    return true;
}

namespace {
void fill(SetClip& slot, const gfx::AnimationClip& clip, const gfx::Skin& skin) {
    slot.clip = clip;
    slot.binding = gfx::bind_clip(slot.clip, skin);
    slot.present = true;
}
} // namespace

void LocomotionSet::set_idle(const gfx::AnimationClip& clip) { fill(idle_, clip, *skin_); }
void LocomotionSet::set_jump_start(const gfx::AnimationClip& clip) { fill(jump_start_, clip, *skin_); }
void LocomotionSet::set_air(const gfx::AnimationClip& clip) { fill(air_, clip, *skin_); }
void LocomotionSet::set_land(const gfx::AnimationClip& clip) { fill(land_, clip, *skin_); }

LocomotionController::LocomotionController(const LocomotionSet& set, LocomotionConfig config) : set_(&set), cfg_(config), pose_(gfx::rest_pose(set.skin())) {
    if (!set.usable()) throw std::invalid_argument("LocomotionController: the set has no locomotion cycle");
    weights_.assign(set.cycles().size(), 0.0f);
    phase_ = set.reference_phase();
    layers_.push_back({LocomotionState::Ground, 0.0f, 1.0f, 0.0f});
}

void LocomotionController::enter(LocomotionState s, float fade) {
    state_ = s;
    state_time_ = 0.0f;
    for (Layer& l : layers_) l.fade = std::max(l.fade, 1e-4f);
    // A layer of the same state that is already fading out is reused rather than duplicated.
    layers_.push_back({s, 0.0f, fade <= 0.0f ? 1.0f : 0.0f, std::max(fade, 1e-4f)});
    if (s == LocomotionState::Ground) phase_ = set_->reference_phase();
}

void LocomotionController::update(float dt, const LocomotionInput& in) {
    if (!(dt > 0.0f) || !std::isfinite(dt) || dt > 1.0f) throw std::invalid_argument("LocomotionController::update: dt must be finite, positive and at most one second");
    if (!std::isfinite(in.speed) || !std::isfinite(in.vertical_speed)) throw std::invalid_argument("LocomotionController::update: the input must be finite");
    const float speed = std::max(in.speed, 0.0f);
    const float k = cfg_.speed_smoothing > 0.0f ? 1.0f - std::exp(-dt / cfg_.speed_smoothing) : 1.0f;
    speed_ += (speed - speed_) * k;

    // The state machine.
    state_time_ += dt;
    switch (state_) {
        case LocomotionState::Ground:
            if (!in.grounded) {
                const bool jump = in.vertical_speed > cfg_.jump_vertical_threshold && set_->jump_start().present;
                enter(jump ? LocomotionState::JumpStart : LocomotionState::Air, cfg_.fade_air);
            }
            break;
        case LocomotionState::JumpStart: {
            const float length = set_->jump_start().clip.duration / cfg_.jump_start_rate;
            if (in.grounded && state_time_ > 0.08f) enter(set_->land().present ? LocomotionState::Land : LocomotionState::Ground, set_->land().present ? cfg_.fade_land : cfg_.fade_ground);
            else if (state_time_ >= length) enter(LocomotionState::Air, cfg_.fade_air);
            break;
        }
        case LocomotionState::Air:
            if (in.grounded) enter(set_->land().present ? LocomotionState::Land : LocomotionState::Ground, set_->land().present ? cfg_.fade_land : cfg_.fade_ground);
            break;
        case LocomotionState::Land: {
            const float length = set_->land().clip.duration / cfg_.land_rate;
            const bool moving = speed_ > cfg_.land_cut_speed;
            if (!in.grounded) enter(LocomotionState::Air, cfg_.fade_air);
            else if (state_time_ >= (moving ? std::min(length, 0.22f) : length)) enter(LocomotionState::Ground, cfg_.fade_ground);
            break;
        }
    }

    // The ground blend's playback: weights over the cycles and the shared phase.
    const std::vector<LocomotionCycle>& cycles = set_->cycles();
    const std::size_t n = cycles.size();
    std::fill(weights_.begin(), weights_.end(), 0.0f);
    float stride = 0.0f;
    idle_weight_ = 0.0f;
    const float s0 = cycles.front().info.ground_speed;
    // Idle or moving, with a hysteresis, and a crossfade of fixed duration between the two whatever the speed does: a capsule accelerates from rest in a few milliseconds, which must not become a pop.
    // While moving, the slowest cycle simply plays slower (its feet still match the ground) down to the idle threshold, because a blend of a standing pose with a half-speed walk never lifts a foot and so skates.
    const bool have_idle = set_->idle().present;
    if (!have_idle) moving_ = true;
    else moving_ = moving_ ? speed_ > cfg_.idle_off * s0 : speed_ > cfg_.idle_on * s0;
    const float loco_target = moving_ ? 1.0f : 0.0f;
    const float slew = dt / std::max(cfg_.fade_idle, 1e-3f);
    loco_ += std::clamp(loco_target - loco_, -slew, slew);
    if (speed_ <= s0) {
        weights_[0] = 1.0f;
        stride = cycles[0].info.stride_length;
    } else if (speed_ >= cycles.back().info.ground_speed) {
        weights_[n - 1] = 1.0f;
        stride = cycles[n - 1].info.stride_length;
    } else {
        std::size_t i = 0;
        while (i + 1 < n && speed_ > cycles[i + 1].info.ground_speed) ++i;
        const float a = cycles[i].info.ground_speed, b = cycles[i + 1].info.ground_speed;
        const float w = (speed_ - a) / (b - a);
        weights_[i] = 1.0f - w;
        weights_[i + 1] = w;
        stride = weights_[i] * cycles[i].info.stride_length + weights_[i + 1] * cycles[i + 1].info.stride_length;
    }
    // Cycles per second so that the blended planted feet move backward at the speed the character has.
    float rate = stride > 1e-4f ? speed_ / stride : 0.0f;
    // The fastest cycle is not played more than max_rate_scale times faster than authored.
    float authored = 0.0f;
    for (std::size_t i = 0; i < n; ++i) authored += weights_[i] / cycles[i].info.duration;
    const float weight_sum = std::max(1e-4f, [&] { float s = 0.0f; for (const float w : weights_) s += w; return s; }());
    authored /= weight_sum;
    rate = cfg_.stride_matching ? std::min(rate, authored * cfg_.max_rate_scale) : authored;
    rate_ = rate;
    stride_speed_ = rate * stride;
    {  // The cycles share what the idle does not take.
        const float l = smoothstep(loco_);
        for (float& w : weights_) w *= l;
        idle_weight_ = have_idle ? 1.0f - l : 0.0f;
    }
    phase_ = fract(phase_ + rate * dt);
    idle_time_ += dt;

    // Layers: the newest fades in, the older ones out.
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        Layer& l = layers_[i];
        l.time += dt;
        const bool newest = i + 1 == layers_.size();
        if (newest) l.weight = std::min(1.0f, l.weight + dt / l.fade);
        else l.weight = std::max(0.0f, l.weight - dt / std::max(layers_.back().fade, 1e-4f));
    }
    // Drop the layers that have faded out (never the newest).
    for (std::size_t i = 0; i + 1 < layers_.size();) {
        if (layers_[i].weight <= 0.0f) layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }

    // Evaluate: the oldest layer first, each newer layer blended over what is below by its own weight.
    pose_ = gfx::rest_pose(set_->skin());
    bool first = true;
    gfx::Pose layer_pose = pose_;
    for (const Layer& l : layers_) {
        evaluate_layer(l, layer_pose);
        if (first) {
            pose_ = layer_pose;
            first = false;
        } else {
            blend_into(pose_, layer_pose, smoothstep(l.weight));
        }
    }
}

void LocomotionController::evaluate_ground(gfx::Pose& out) {
    const std::vector<LocomotionCycle>& cycles = set_->cycles();
    bool have = false;
    float total = 0.0f;
    const auto add = [&](const gfx::Pose& p, float w) {
        if (w <= 0.0f) return;
        if (!have) {
            out = p;
            have = true;
            total = w;
        } else {
            total += w;
            blend_into(out, p, w / total);
        }
    };
    gfx::Pose tmp = gfx::rest_pose(set_->skin());
    for (std::size_t i = 0; i < cycles.size(); ++i) {
        if (weights_[i] <= 0.0f) continue;
        const LocomotionCycle& c = cycles[i];
        // Aligned phase: the left foot of every cycle lands at the same shared phase.
        const float aligned = fract(phase_ - set_->reference_phase() + c.info.left.contact_start);
        tmp = gfx::rest_pose(set_->skin());
        evaluate_in_place(set_->skin(), set_->map(), set_->rig(), c.clip, c.binding, aligned * c.clip.duration, c.info, tmp);
        add(tmp, weights_[i]);
    }
    if (idle_weight_ > 0.0f && set_->idle().present) {
        tmp = gfx::rest_pose(set_->skin());
        gfx::evaluate_clip(set_->idle().clip, set_->idle().binding, idle_time_, true, tmp);
        add(tmp, idle_weight_);
    }
    if (!have) out = gfx::rest_pose(set_->skin());
}

void LocomotionController::evaluate_layer(const Layer& layer, gfx::Pose& out) {
    out = gfx::rest_pose(set_->skin());
    switch (layer.state) {
        case LocomotionState::Ground: evaluate_ground(out); break;
        case LocomotionState::JumpStart:
            gfx::evaluate_clip(set_->jump_start().clip, set_->jump_start().binding, layer.time * cfg_.jump_start_rate, false, out);
            break;
        case LocomotionState::Air:
            if (set_->air().present) gfx::evaluate_clip(set_->air().clip, set_->air().binding, layer.time, true, out);
            break;
        case LocomotionState::Land:
            gfx::evaluate_clip(set_->land().clip, set_->land().binding, layer.time * cfg_.land_rate, false, out);
            break;
    }
}

} // namespace saige::anim
