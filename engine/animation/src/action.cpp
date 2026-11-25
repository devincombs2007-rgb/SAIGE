#include "saige/animation/action.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

namespace {

float dot2(render::Float3 a, render::Float3 b) { return a.x * b.x + a.y * b.y; }

} // namespace

ActionClip::ActionClip(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, float sample_rate, bool vertical) : skin_(&skin), rig_(rig), clip_(clip), vertical_(vertical) {
    binding_ = gfx::bind_clip(clip_, skin);
    if (!(clip_.duration > 0.0f) || !(sample_rate > 0.0f) || !std::isfinite(sample_rate)) return;
    const int n = std::max(2, static_cast<int>(std::ceil(clip_.duration * sample_rate)));
    step_ = clip_.duration / static_cast<float>(n);
    path_.reserve(static_cast<std::size_t>(n) + 1);
    gfx::Pose pose = gfx::rest_pose(skin);
    const std::size_t hips = static_cast<std::size_t>(map[Bone::Hips]);
    render::Float3 origin{};
    for (int i = 0; i <= n; ++i) {
        gfx::evaluate_clip(clip_, binding_, step_ * static_cast<float>(i), false, pose);
        const render::Float3 p = joint_world(skin, pose)[hips].position;
        if (i == 0) origin = p;
        const render::Float3 d = p - origin;
        path_.push_back({dot2(d, rig.forward), dot2(d, rig.left), vertical ? d.z : 0.0f});
    }
    total_ = path_.back();
    valid_ = true;
}

float ActionClip::travel_length() const noexcept { return std::sqrt(total_.forward * total_.forward + total_.left * total_.left + total_.up * total_.up); }

PlanarVelocity ActionClip::offset(float time) const {
    if (path_.empty()) return {};
    if (!std::isfinite(time) || time <= 0.0f) return path_.front();
    const float x = time / step_;
    const std::size_t i = static_cast<std::size_t>(x);
    if (i + 1 >= path_.size()) return path_.back();
    const float f = x - static_cast<float>(i);
    return {path_[i].forward + (path_[i + 1].forward - path_[i].forward) * f, path_[i].left + (path_[i + 1].left - path_[i].left) * f, path_[i].up + (path_[i + 1].up - path_[i].up) * f};
}

void ActionPlayer::start(const ActionClip& clip) {
    if (!clip.valid()) throw std::invalid_argument("ActionPlayer::start: the clip is not valid (no duration)");
    clip_ = &clip;
    time_ = since_start_ = since_cancel_ = stall_time_ = 0.0f;
    cancelled_ = stalled_ = done_ = false;
    weight_ = cfg_.fade_in > 0.0f ? 0.0f : 1.0f;
    rest_ = gfx::rest_pose(clip.skin());
    pose_ = rest_;
    evaluate();
}

void ActionPlayer::cancel() {
    if (!active() || cancelled_) return;
    cancelled_ = true;
    since_cancel_ = 0.0f;
}

PlanarVelocity ActionPlayer::command(float dt) const {
    if (!travelling() || !(dt > 0.0f) || !std::isfinite(dt)) return {};
    const float t1 = std::min(time_ + dt, clip_->duration());
    const PlanarVelocity a = clip_->offset(time_), b = clip_->offset(t1);
    return {(b.forward - a.forward) / dt, (b.left - a.left) / dt, (b.up - a.up) / dt};
}

void ActionPlayer::advance(float dt, float progress) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) throw std::invalid_argument("ActionPlayer::advance: dt must be finite and positive");
    if (!active()) return;
    const float p = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    since_start_ += dt;
    if (cancelled_) {
        since_cancel_ += dt;
    } else {
        time_ = std::min(time_ + dt * p, clip_->duration());
        // Headway is only judged where the clip actually travels: a pause in the middle of a roll is not a stall.
        const PlanarVelocity v = command(dt);
        const bool travelling_now = std::sqrt(v.forward * v.forward + v.left * v.left + v.up * v.up) > 0.05f;
        stall_time_ = (travelling_now && p < cfg_.stall_progress) ? stall_time_ + dt : 0.0f;
        if (stall_time_ >= cfg_.stall_seconds) {
            stalled_ = true;
            cancelled_ = true;
            since_cancel_ = 0.0f;
        }
    }
    // The weight: in over fade_in from the start, out over fade_out before the clip ends or after a cancel.
    float w = cfg_.fade_in > 0.0f ? std::min(1.0f, since_start_ / cfg_.fade_in) : 1.0f;
    if (cancelled_) {
        w = std::min(w, cfg_.fade_out > 0.0f ? 1.0f - since_cancel_ / cfg_.fade_out : 0.0f);
    } else if (cfg_.fade_out > 0.0f) {
        w = std::min(w, (clip_->duration() - time_) / cfg_.fade_out);
    } else if (time_ >= clip_->duration()) {
        w = 0.0f;
    }
    weight_ = std::clamp(w, 0.0f, 1.0f);
    if ((cancelled_ && weight_ <= 0.0f) || (!cancelled_ && time_ >= clip_->duration() && weight_ <= 0.0f)) {
        done_ = true;
        weight_ = 0.0f;
        return;
    }
    evaluate();
}

void ActionPlayer::evaluate() {
    const gfx::Skin& skin = clip_->skin();
    pose_ = rest_;
    gfx::evaluate_clip(clip_->clip(), clip_->binding(), time_, false, pose_);
    // The hips' own horizontal travel at this time, removed exactly (not a linear drift): the character's position is the physics'.
    const PlanarVelocity o = clip_->offset(time_);
    const RigInfo& rig = clip_->rig();
    const render::Float3 travel = rig.forward * o.forward + rig.left * o.left;
    translate_model(skin, {-travel.x, -travel.y, -o.up}, pose_);
}

} // namespace saige::anim
