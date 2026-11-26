#include "saige/env/wind.hpp"

#include "saige/env/random.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::env {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr double kOmega[3] = {0.37, 0.91, 2.3};  // rad/s: swells over 17 s, gusts over 7 s, flutter over 2.7 s
constexpr double kWeight[3] = {0.5, 0.3, 0.2};   // sums to 1, so the factor stays inside its bounds
} // namespace

WindField::WindField(std::uint64_t seed)
{
    for (int i = 0; i < 3; ++i) phase_[i] = kTwoPi * unit(hash_combine(seed, static_cast<std::uint64_t>(i) + std::uint64_t{0x57494E44}));
}

void WindField::set(double mean_speed, double direction, double gustiness)
{
    if (!std::isfinite(mean_speed) || mean_speed < 0.0 || !std::isfinite(direction) || !std::isfinite(gustiness) || gustiness < 0.0 || gustiness > 1.0)
        throw std::invalid_argument("WindField::set: the speed must be finite and >= 0, the direction finite and the gustiness in [0, 1]");
    mean_ = mean_speed;
    direction_ = direction;
    gustiness_ = gustiness;
}

double WindField::gust_factor(double t, double x, double y) const
{
    // The pattern travels downwind at the mean speed (but not slower than 1 m/s, so a calm does not freeze it in place).
    const double travel = std::max(mean_, 1.0);
    const double along = x * std::cos(direction_) + y * std::sin(direction_);
    const double tau = t - along / travel;
    double s = 0.0;
    for (int i = 0; i < 3; ++i) s += kWeight[i] * std::sin(kOmega[i] * tau + phase_[i]);
    return 1.0 + gustiness_ * s;
}

WindSample WindField::at(double x, double y, double t) const
{
    const double speed = mean_ * gust_factor(t, x, y);
    return {speed * std::cos(direction_), speed * std::sin(direction_), speed};
}

} // namespace saige::env
