// Wind: a mean wind plus gusts that travel with it. Gusts follow Taylor's frozen-turbulence idea: the gust pattern is advected by the mean wind, so a gust that reaches one tree reaches the next one
// downwind a moment later and a field of grass shows waves rolling across it. A sum of three incommensurate sinusoids with seeded phases gives a bounded, smooth, non-repeating-looking factor.
// Deterministic: the same seed, position and time give the same wind.
#pragma once

#include <cstdint>

namespace saige::env {

struct WindSample {
    double x = 0.0, y = 0.0;  ///< m/s, the velocity of the air
    double speed = 0.0;       ///< m/s, the length of (x, y)
};

class WindField {
public:
    explicit WindField(std::uint64_t seed = 1);
    /// Mean speed (m/s, >= 0), the direction the wind blows toward (radians, 0 is +x, counter-clockwise) and the gust strength in [0, 1]: 0 is a steady wind, 1 swings the speed between 0 and twice the mean.
    void set(double mean_speed, double direction, double gustiness);
    [[nodiscard]] double mean_speed() const noexcept { return mean_; }
    [[nodiscard]] double direction() const noexcept { return direction_; }
    [[nodiscard]] double gustiness() const noexcept { return gustiness_; }
    /// The gust factor at time t (seconds) at the position's advection offset: 1 + gustiness * s, with s in [-1, 1], so it lies in [1 - gustiness, 1 + gustiness].
    [[nodiscard]] double gust_factor(double t, double x = 0.0, double y = 0.0) const;
    [[nodiscard]] WindSample at(double x, double y, double t) const;

private:
    double phase_[3]{};
    double mean_ = 0.0, direction_ = 0.0, gustiness_ = 0.0;
};

} // namespace saige::env
