// Dynamic weather as a small deterministic state machine with smooth, continuous outputs.
//
// A phase graph (Clear, Overcast, Drizzle, Rain, Storm, Clearing) with seeded dwell times and neighbour-only transitions (the weather never jumps from Clear to Storm). Each phase has a target for the
// rain intensity, the cloud cover and the mean wind; the state approaches the target with first-order lags (rain rises faster than it fades), so every output is continuous in time. From the state the
// module derives what consumers need: the rain rate in mm/h, the air's optical extinction and the meteorological visibility (Koschmieder, with the Marshall-Palmer-style rain term), a wind field with gusts,
// and the inputs of the audio ambience model. Nothing here draws or plays anything; the renderer, the audio mixer and the water module read the numbers.
#pragma once

#include "saige/env/wind.hpp"

#include <cstdint>

namespace saige::env {

enum class WeatherPhase { Clear, Overcast, Drizzle, Rain, Storm, Clearing };
inline constexpr int kWeatherPhaseCount = 6;
[[nodiscard]] const char* phase_name(WeatherPhase phase) noexcept;
/// Whether the weather may move from `from` to `to` in one step (and `from == to` is not a move).
[[nodiscard]] bool phase_transition_allowed(WeatherPhase from, WeatherPhase to) noexcept;

struct WeatherTarget {
    double rain = 0.0;        ///< 0..1
    double cloud = 0.0;       ///< 0..1 cover
    double wind_speed = 0.0;  ///< m/s, mean
    double gustiness = 0.0;   ///< 0..1
};
[[nodiscard]] WeatherTarget phase_target(WeatherPhase phase) noexcept;

struct WeatherConfig {
    std::uint64_t seed = 1;
    WeatherPhase start = WeatherPhase::Clear;
    double dwell_min_seconds = 240.0;  ///< a phase lasts at least this long
    double dwell_max_seconds = 900.0;  ///< and at most this long
    double rain_rise_tau = 45.0;       ///< seconds; rain builds up faster than it dies away
    double rain_fall_tau = 120.0;
    double cloud_tau = 150.0;
    double wind_tau = 60.0;
    double wind_direction = 0.0;       ///< radians the wind blows toward (0 is +x)
    double base_visibility_m = 20000.0;///< clear-air meteorological visibility
};

struct WeatherState {
    WeatherPhase phase = WeatherPhase::Clear;
    double time = 0.0;               ///< seconds since the start
    double rain = 0.0;               ///< 0..1 intensity
    double rain_mm_per_hour = 0.0;   ///< derived: 50 * rain^2 (drizzle about 3, heavy storm 50)
    double cloud = 0.0;              ///< 0..1
    double wind_speed = 0.0;         ///< m/s, mean
    double extinction = 0.0;         ///< 1/m, optical, haze plus rain
    double visibility_m = 0.0;       ///< 3.912 / extinction
    double sun_visibility = 1.0;     ///< 0..1: how much of the direct sun reaches the ground (cloud and rain)
};

/// The rain rate of an intensity.
[[nodiscard]] double rain_rate_mm_per_hour(double rain) noexcept;
/// Koschmieder: the distance at which the contrast of a black object falls to 2%.
[[nodiscard]] double visibility_from_extinction(double extinction_per_m) noexcept;

/// What the audio ambience model (`ambience_mix` in saige/audio_model.hpp) takes from the weather.
struct WeatherAudio {
    double wind_speed = 0.0;  ///< m/s, with the current gust
    double rain = 0.0;        ///< 0..1
};

class WeatherSystem {
public:
    explicit WeatherSystem(const WeatherConfig& config = {});
    /// Advance by dt seconds (finite, >= 0). Large steps are split internally, so the result does not depend on how time is sliced beyond the first-order lag's own exactness.
    void step(double dt);
    [[nodiscard]] const WeatherState& state() const noexcept { return state_; }
    [[nodiscard]] const WindField& wind() const noexcept { return wind_; }
    [[nodiscard]] WeatherAudio audio(double x = 0.0, double y = 0.0) const;
    /// Seconds left in the current phase.
    [[nodiscard]] double phase_remaining() const noexcept { return phase_end_ - state_.time; }
    /// Move to another phase now (it must be reachable from the current one in one step); used by a host that scripts a storm. The dwell restarts. Throws std::invalid_argument otherwise.
    void force_phase(WeatherPhase next);

private:
    void choose_next();
    double dwell_for(std::uint64_t index) const;
    WeatherConfig cfg_;
    WeatherState state_;
    WindField wind_;
    double phase_end_ = 0.0;
    std::uint64_t index_ = 0;  ///< the number of phase changes so far (selects the next draw)
};

} // namespace saige::env
