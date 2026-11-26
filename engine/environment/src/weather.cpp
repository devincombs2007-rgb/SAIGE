#include "saige/env/weather.hpp"

#include "saige/env/random.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::env {

namespace {

constexpr double kMaxSubstep = 5.0;

/// The neighbours of each phase, in a fixed order (the draw indexes into this list).
struct Neighbours {
    int count;
    WeatherPhase to[3];
};
Neighbours neighbours(WeatherPhase p)
{
    switch (p) {
    case WeatherPhase::Clear: return {1, {WeatherPhase::Overcast, WeatherPhase::Overcast, WeatherPhase::Overcast}};
    case WeatherPhase::Overcast: return {2, {WeatherPhase::Clear, WeatherPhase::Drizzle, WeatherPhase::Drizzle}};
    case WeatherPhase::Drizzle: return {3, {WeatherPhase::Overcast, WeatherPhase::Rain, WeatherPhase::Clearing}};
    case WeatherPhase::Rain: return {3, {WeatherPhase::Drizzle, WeatherPhase::Storm, WeatherPhase::Clearing}};
    case WeatherPhase::Storm: return {1, {WeatherPhase::Rain, WeatherPhase::Rain, WeatherPhase::Rain}};
    case WeatherPhase::Clearing: return {2, {WeatherPhase::Clear, WeatherPhase::Overcast, WeatherPhase::Overcast}};
    }
    return {1, {WeatherPhase::Clear, WeatherPhase::Clear, WeatherPhase::Clear}};
}

/// The exact solution of x' = (target - x) / tau over dt.
double lag(double x, double target, double tau, double dt) { return target + (x - target) * std::exp(-dt / tau); }

} // namespace

const char* phase_name(WeatherPhase phase) noexcept
{
    switch (phase) {
    case WeatherPhase::Clear: return "clear";
    case WeatherPhase::Overcast: return "overcast";
    case WeatherPhase::Drizzle: return "drizzle";
    case WeatherPhase::Rain: return "rain";
    case WeatherPhase::Storm: return "storm";
    case WeatherPhase::Clearing: return "clearing";
    }
    return "?";
}

bool phase_transition_allowed(WeatherPhase from, WeatherPhase to) noexcept
{
    if (from == to) return false;
    const Neighbours n = neighbours(from);
    for (int i = 0; i < n.count; ++i)
        if (n.to[i] == to) return true;
    return false;
}

WeatherTarget phase_target(WeatherPhase phase) noexcept
{
    switch (phase) {
    case WeatherPhase::Clear: return {0.0, 0.10, 3.0, 0.20};
    case WeatherPhase::Overcast: return {0.0, 0.75, 5.0, 0.30};
    case WeatherPhase::Drizzle: return {0.25, 0.85, 6.0, 0.30};
    case WeatherPhase::Rain: return {0.60, 0.95, 9.0, 0.45};
    case WeatherPhase::Storm: return {1.00, 1.00, 17.0, 0.70};
    case WeatherPhase::Clearing: return {0.0, 0.50, 7.0, 0.40};
    }
    return {};
}

double rain_rate_mm_per_hour(double rain) noexcept
{
    const double r = std::clamp(rain, 0.0, 1.0);
    return 50.0 * r * r;
}

double visibility_from_extinction(double extinction_per_m) noexcept { return extinction_per_m > 0.0 ? 3.912 / extinction_per_m : 1e12; }

WeatherSystem::WeatherSystem(const WeatherConfig& config) : cfg_(config), wind_(config.seed)
{
    if (!(cfg_.dwell_min_seconds > 0.0) || !(cfg_.dwell_max_seconds >= cfg_.dwell_min_seconds) || !(cfg_.rain_rise_tau > 0.0) || !(cfg_.rain_fall_tau > 0.0) || !(cfg_.cloud_tau > 0.0) ||
        !(cfg_.wind_tau > 0.0) || !(cfg_.base_visibility_m > 0.0) || !std::isfinite(cfg_.wind_direction))
        throw std::invalid_argument("WeatherSystem: the dwell range and time constants must be positive, the visibility positive and the wind direction finite");
    state_.phase = cfg_.start;
    const WeatherTarget t = phase_target(cfg_.start);
    state_.rain = t.rain;
    state_.cloud = t.cloud;
    state_.wind_speed = t.wind_speed;
    phase_end_ = dwell_for(0);
    wind_.set(state_.wind_speed, cfg_.wind_direction, t.gustiness);
    step(0.0);
}

double WeatherSystem::dwell_for(std::uint64_t index) const
{
    return cfg_.dwell_min_seconds + (cfg_.dwell_max_seconds - cfg_.dwell_min_seconds) * unit(hash_combine(cfg_.seed, std::uint64_t{0x4457454C} + index));
}

void WeatherSystem::choose_next()
{
    const Neighbours n = neighbours(state_.phase);
    const std::uint64_t draw = hash_combine(cfg_.seed, std::uint64_t{0x4E455854} + index_);
    state_.phase = n.to[draw % static_cast<std::uint64_t>(n.count)];
    ++index_;
    phase_end_ = state_.time + dwell_for(index_);
}

void WeatherSystem::force_phase(WeatherPhase next)
{
    if (!phase_transition_allowed(state_.phase, next)) throw std::invalid_argument("WeatherSystem::force_phase: the phase is not reachable in one step");
    state_.phase = next;
    ++index_;
    phase_end_ = state_.time + dwell_for(index_);
}

void WeatherSystem::step(double dt)
{
    if (!std::isfinite(dt) || dt < 0.0) throw std::invalid_argument("WeatherSystem::step: dt must be finite and >= 0");
    double left = dt;
    do {
        const double h = std::min(left, kMaxSubstep);
        left -= h;
        state_.time += h;
        while (state_.time >= phase_end_) choose_next();
        const WeatherTarget t = phase_target(state_.phase);
        const double rain_tau = t.rain > state_.rain ? cfg_.rain_rise_tau : cfg_.rain_fall_tau;
        state_.rain = lag(state_.rain, t.rain, rain_tau, h);
        state_.cloud = lag(state_.cloud, t.cloud, cfg_.cloud_tau, h);
        state_.wind_speed = lag(state_.wind_speed, t.wind_speed, cfg_.wind_tau, h);
        const double gust = wind_.gustiness();
        wind_.set(state_.wind_speed, cfg_.wind_direction, lag(gust, t.gustiness, cfg_.wind_tau, h));
    } while (left > 0.0);

    state_.rain_mm_per_hour = rain_rate_mm_per_hour(state_.rain);
    // Haze (clear air plus humidity under cloud) and rain: the rain term follows beta = 0.365 R^0.63 per km (R in mm/h), the usual optical fit to a Marshall-Palmer drop spectrum.
    const double haze = 3.912 / cfg_.base_visibility_m * (1.0 + 1.5 * state_.cloud);
    const double rain_term = state_.rain_mm_per_hour > 0.0 ? 0.365e-3 * std::pow(state_.rain_mm_per_hour, 0.63) : 0.0;
    state_.extinction = haze + rain_term;
    state_.visibility_m = visibility_from_extinction(state_.extinction);
    state_.sun_visibility = std::clamp((1.0 - state_.cloud) * (1.0 - 0.5 * state_.rain), 0.0, 1.0);
}

WeatherAudio WeatherSystem::audio(double x, double y) const
{
    return {wind_.at(x, y, state_.time).speed, state_.rain};
}

} // namespace saige::env
