// Weather: determinism, the transition graph, continuity, derived visibility, and the wind field.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/env/weather.hpp"

#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace saige::env;

namespace {
template <class F>
bool throws(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
} // namespace

TEST_CASE("weather: the same seed and step sequence give bit-identical states and different seeds diverge") {
    WeatherConfig a, b;
    a.seed = b.seed = 7;
    WeatherSystem wa(a), wb(b);
    WeatherConfig c;
    c.seed = 8;
    WeatherSystem wc(c);
    bool differ = false;
    for (int i = 0; i < 6 * 3600; ++i) {
        wa.step(1.0);
        wb.step(1.0);
        wc.step(1.0);
        REQUIRE(wa.state().rain == wb.state().rain);
        REQUIRE(wa.state().phase == wb.state().phase);
        REQUIRE(wa.state().visibility_m == wb.state().visibility_m);
        if (wa.state().phase != wc.state().phase) differ = true;
    }
    CHECK(differ);
}

TEST_CASE("weather: phases only move along the graph's edges, and every phase is reachable") {
    std::set<int> seen;
    for (std::uint64_t seed = 1; seed <= 40 && seen.size() < kWeatherPhaseCount; ++seed) {
        WeatherConfig cfg;
        cfg.seed = seed;
        WeatherSystem w(cfg);
        WeatherPhase prev = w.state().phase;
        seen.insert(static_cast<int>(prev));
        for (int i = 0; i < 24 * 3600; ++i) {
            w.step(1.0);
            const WeatherPhase now = w.state().phase;
            if (now != prev) {
                REQUIRE(phase_transition_allowed(prev, now));
                prev = now;
                seen.insert(static_cast<int>(now));
            }
        }
    }
    CHECK(seen.size() == static_cast<std::size_t>(kWeatherPhaseCount));
    CHECK_FALSE(phase_transition_allowed(WeatherPhase::Clear, WeatherPhase::Storm));
    CHECK_FALSE(phase_transition_allowed(WeatherPhase::Storm, WeatherPhase::Clear));
    CHECK_FALSE(phase_transition_allowed(WeatherPhase::Rain, WeatherPhase::Rain));
}

TEST_CASE("weather: outputs are continuous (no jumps at phase changes) and stay in range") {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        WeatherConfig cfg;
        cfg.seed = seed;
        WeatherSystem w(cfg);
        WeatherState prev = w.state();
        for (int i = 0; i < 12 * 3600; ++i) {
            w.step(1.0);
            const WeatherState& s = w.state();
            // The fastest lag is rain rising with a 45 s constant toward at most 1: 1/45 per second.
            REQUIRE(std::abs(s.rain - prev.rain) < 0.025);
            REQUIRE(std::abs(s.cloud - prev.cloud) < 0.01);
            REQUIRE(std::abs(s.wind_speed - prev.wind_speed) < 0.5);
            REQUIRE(s.rain >= 0.0);
            REQUIRE(s.rain <= 1.0);
            REQUIRE(s.cloud >= 0.0);
            REQUIRE(s.cloud <= 1.0);
            REQUIRE(s.visibility_m > 0.0);
            REQUIRE(s.visibility_m <= cfg.base_visibility_m);
            REQUIRE(s.sun_visibility >= 0.0);
            REQUIRE(s.sun_visibility <= 1.0);
            prev = s;
        }
    }
}

TEST_CASE("weather: rain rate, Koschmieder visibility and the storm's effect on sight distance") {
    CHECK(rain_rate_mm_per_hour(0.0) == 0.0);
    CHECK(rain_rate_mm_per_hour(1.0) == 50.0);
    CHECK(rain_rate_mm_per_hour(0.5) < rain_rate_mm_per_hour(0.6));
    CHECK(std::abs(visibility_from_extinction(3.912 / 20000.0) - 20000.0) < 1e-6);
    CHECK(visibility_from_extinction(0.0) > 1e9);

    WeatherConfig cfg;
    cfg.seed = 3;
    cfg.start = WeatherPhase::Rain;
    WeatherSystem w(cfg);
    w.force_phase(WeatherPhase::Storm);
    for (int i = 0; i < 120; ++i) w.step(1.0);  // inside the storm's minimum dwell of 240 s
    CHECK(w.state().phase == WeatherPhase::Storm);
    CHECK(w.state().rain > 0.9);
    CHECK(w.state().visibility_m < 3000.0);
    WeatherConfig clear;
    clear.seed = 3;
    WeatherSystem c(clear);
    c.step(1.0);
    CHECK(c.state().visibility_m > 10.0 * w.state().visibility_m);
    CHECK(c.state().sun_visibility > w.state().sun_visibility);
}

TEST_CASE("weather: forcing a phase obeys the graph, and a forced phase restarts the dwell") {
    WeatherConfig cfg;
    cfg.seed = 5;
    WeatherSystem w(cfg);
    CHECK(throws([&] { w.force_phase(WeatherPhase::Storm); }));
    CHECK(throws([&] { w.force_phase(WeatherPhase::Clear); }));
    w.force_phase(WeatherPhase::Overcast);
    CHECK(w.state().phase == WeatherPhase::Overcast);
    CHECK(w.phase_remaining() >= cfg.dwell_min_seconds);
    CHECK(w.phase_remaining() <= cfg.dwell_max_seconds);
    CHECK(throws([&] { w.step(-1.0); }));
    CHECK(throws([&] { w.step(std::nan("")); }));
    WeatherConfig bad;
    bad.dwell_max_seconds = 10.0;
    CHECK(throws([&] { WeatherSystem x(bad); }));
}

TEST_CASE("wind: gusts stay inside their bounds, are steady when gustiness is 0, and travel downwind with the mean wind") {
    WindField steady(9);
    steady.set(6.0, 0.3, 0.0);
    for (double t = 0.0; t < 100.0; t += 0.7) CHECK(steady.at(3.0, 4.0, t).speed == 6.0);

    WindField w(9);
    w.set(8.0, 1.0, 0.6);
    double lo = 1e9, hi = -1e9;
    for (double t = 0.0; t < 600.0; t += 0.25) {
        const double g = w.gust_factor(t);
        lo = std::min(lo, g);
        hi = std::max(hi, g);
        CHECK(g >= 1.0 - 0.6 - 1e-12);
        CHECK(g <= 1.0 + 0.6 + 1e-12);
        const WindSample s = w.at(0, 0, t);
        CHECK(std::abs(std::hypot(s.x, s.y) - s.speed) < 1e-9);
    }
    CHECK(hi - lo > 0.5);  // it does swing
    // A point d metres downwind sees what the origin saw d / speed seconds earlier.
    const double dx = std::cos(1.0), dy = std::sin(1.0), d = 40.0;
    for (double t = 10.0; t < 50.0; t += 3.1) CHECK(std::abs(w.gust_factor(t, d * dx, d * dy) - w.gust_factor(t - d / 8.0)) < 1e-12);
    CHECK(throws([&] { w.set(-1.0, 0.0, 0.0); }));
    CHECK(throws([&] { w.set(1.0, 0.0, 1.5); }));
}

TEST_CASE("weather: the audio inputs mirror the state and respond to the storm") {
    WeatherConfig cfg;
    cfg.seed = 3;
    cfg.start = WeatherPhase::Rain;
    WeatherSystem w(cfg);
    w.force_phase(WeatherPhase::Storm);
    for (int i = 0; i < 120; ++i) w.step(1.0);
    const WeatherAudio a = w.audio();
    CHECK(a.rain == w.state().rain);
    CHECK(a.wind_speed > 5.0);
}
