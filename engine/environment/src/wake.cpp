#include "saige/env/wake.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::env {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

double froude_number(double speed, double length_m)
{
    if (!(length_m > 0.0) || !(speed >= 0.0) || !std::isfinite(speed) || !std::isfinite(length_m)) throw std::invalid_argument("froude_number: the length must be positive and the speed >= 0, both finite");
    return speed / std::sqrt(kGravity * length_m);
}

double hull_speed(double length_m)
{
    if (!(length_m > 0.0) || !std::isfinite(length_m)) throw std::invalid_argument("hull_speed: the length must be positive and finite");
    return 0.40 * std::sqrt(kGravity * length_m);
}

double transverse_wavelength(double speed)
{
    if (!(speed >= 0.0) || !std::isfinite(speed)) throw std::invalid_argument("transverse_wavelength: the speed must be finite and >= 0");
    return kTwoPi * speed * speed / kGravity;
}

double kelvin_half_width(double distance_behind) { return std::max(distance_behind, 0.0) * std::tan(kKelvinHalfAngleRadians); }

WakeTrail::WakeTrail(const WakeConfig& config) : cfg_(config)
{
    if (!(cfg_.hull_length > 0.0) || !(cfg_.hull_beam > 0.0) || !(cfg_.foam_seconds > 0.0) || !(cfg_.spawn_interval > 0.0) || !(cfg_.min_speed >= 0.0) || !(cfg_.spread_speed >= 0.0) || cfg_.max_patches == 0)
        throw std::invalid_argument("WakeConfig: the hull size, foam time and spawn interval must be positive, the speeds >= 0 and the patch limit non-zero");
}

void WakeTrail::update(double t, double x, double y, double speed)
{
    if (!std::isfinite(t) || !std::isfinite(x) || !std::isfinite(y) || !(speed >= 0.0) || !std::isfinite(speed)) throw std::invalid_argument("WakeTrail::update: the arguments must be finite and the speed >= 0");
    if (started_ && t < last_t_) throw std::invalid_argument("WakeTrail::update: time must not decrease");
    if (started_) {
        travelled_ += std::hypot(x - last_x_, y - last_y_);
    }
    started_ = true;
    last_x_ = x;
    last_y_ = y;
    last_t_ = t;
    if (speed >= cfg_.min_speed && (patches_.empty() || travelled_ >= cfg_.spawn_interval)) {
        const double fraction = std::clamp(speed / hull_speed(cfg_.hull_length), 0.0, 1.0);
        patches_.push_back({x, y, t, std::pow(fraction, 1.5)});
        travelled_ = 0.0;
    }
    // Drop patches that have faded below 1% of any possible strength (about 4.6 time constants).
    const double lifetime = 4.6 * cfg_.foam_seconds;
    const auto old = std::remove_if(patches_.begin(), patches_.end(), [&](const Patch& p) { return t - p.t > lifetime; });
    patches_.erase(old, patches_.end());
    if (patches_.size() > cfg_.max_patches) patches_.erase(patches_.begin(), patches_.begin() + static_cast<std::ptrdiff_t>(patches_.size() - cfg_.max_patches));
}

double WakeTrail::foam(double x, double y, double t) const
{
    double sum = 0.0;
    for (const Patch& p : patches_) {
        const double age = t - p.t;
        if (age < 0.0) continue;
        const double sigma = 0.5 * cfg_.hull_beam + cfg_.spread_speed * age;
        const double dx = x - p.x, dy = y - p.y;
        const double r2 = dx * dx + dy * dy;
        if (r2 > 9.0 * sigma * sigma) continue;
        // Spreading conserves the foam's amount: the peak falls as the patch widens (by sigma0 / sigma squared).
        const double s0 = 0.5 * cfg_.hull_beam;
        // The patches overlap along the track; the factor spacing / (s0 sqrt(2 pi)) makes a line of them add up to a density of 1 at full strength, on the track, when fresh.
        const double line = cfg_.spawn_interval / (2.5066282746310002 * s0);
        sum += line * p.strength * std::exp(-age / cfg_.foam_seconds) * (s0 * s0) / (sigma * sigma) * std::exp(-0.5 * r2 / (sigma * sigma));
    }
    return std::clamp(sum, 0.0, 1.0);
}

} // namespace saige::env
