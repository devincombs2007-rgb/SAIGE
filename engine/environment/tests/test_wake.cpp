// Wake geometry and the foam trail.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/env/wake.hpp"

#include <stdexcept>
#include <cmath>

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

/// Drive a boat along +x at `speed` from t = 0 for `seconds`; returns the final x.
double cruise(WakeTrail& wake, double speed, double seconds, double dt = 0.1)
{
    double x = 0.0;
    for (double t = 0.0; t <= seconds + 1e-9; t += dt) {
        wake.update(t, x, 0.0, speed);
        x += speed * dt;
    }
    return x;
}
} // namespace

TEST_CASE("wake: the Kelvin angle, hull speed and transverse wavelength follow the textbook values") {
    CHECK(std::abs(kKelvinHalfAngleRadians - std::asin(1.0 / 3.0)) < 1e-15);
    CHECK(std::abs(kKelvinHalfAngleRadians * 180.0 / 3.14159265358979323846 - 19.4712) < 1e-3);
    CHECK(std::abs(kelvin_half_width(10.0) - 10.0 / (2.0 * std::sqrt(2.0))) < 1e-9);
    CHECK(kelvin_half_width(-3.0) == 0.0);
    CHECK(std::abs(froude_number(hull_speed(6.0), 6.0) - 0.40) < 1e-12);
    CHECK(std::abs(hull_speed(10.0) - 1.25 * std::sqrt(10.0)) < 0.02);  // 1.25 sqrt(L) m/s
    CHECK(std::abs(transverse_wavelength(5.0) - 2.0 * 3.14159265358979323846 * 25.0 / 9.81) < 1e-9);
    CHECK(transverse_wavelength(0.0) == 0.0);
    CHECK(throws([&] { (void)hull_speed(0.0); }));
    CHECK(throws([&] { (void)froude_number(-1.0, 5.0); }));
}

TEST_CASE("wake: a stationary or slow hull sheds no foam, a moving one leaves a trail behind it only") {
    WakeTrail idle;
    for (double t = 0.0; t < 30.0; t += 0.1) idle.update(t, 0.0, 0.0, 0.0);
    CHECK(idle.patch_count() == 0);
    CHECK(idle.foam(0.0, 0.0, 30.0) == 0.0);

    WakeTrail wake;
    const double end = cruise(wake, 6.0, 20.0);
    const double t = 20.0;
    CHECK(wake.foam(end - 5.0, 0.0, t) > 0.2);        // on the track, behind the hull
    CHECK(wake.foam(end - 5.0, 20.0, t) == 0.0);      // far to the side
    CHECK(wake.foam(end + 12.0, 0.0, t) == 0.0);      // ahead of the hull
    CHECK(wake.foam(end - 5.0, 1.5, t) < wake.foam(end - 5.0, 0.0, t));  // thins away from the track
    for (double x = -20.0; x < 150.0; x += 3.7) {
        const double f = wake.foam(x, 0.3, t);
        REQUIRE(f >= 0.0);
        REQUIRE(f <= 1.0);
    }
}

TEST_CASE("wake: foam decays after the boat has passed, and a faster boat leaves more") {
    WakeTrail wake;
    const double end = cruise(wake, 6.0, 20.0);
    const double px = end - 30.0;
    double prev = wake.foam(px, 0.0, 20.0);
    REQUIRE(prev > 0.1);
    for (const double t : {30.0, 45.0, 60.0, 90.0, 140.0}) {
        const double f = wake.foam(px, 0.0, t);
        REQUIRE(f < prev);
        prev = f;
    }
    CHECK(prev < 0.01 * wake.foam(px, 0.0, 20.0) + 1e-3);

    WakeTrail slow, fast;
    const double e1 = cruise(slow, 1.5, 20.0), e2 = cruise(fast, 6.0, 20.0);
    CHECK(wake.foam(e2 - 3.0, 0.0, 20.0) > 0.0);
    CHECK(fast.foam(e2 - 3.0, 0.0, 20.0) > slow.foam(e1 - 3.0, 0.0, 20.0));
}

TEST_CASE("wake: patches are dropped when they have faded, the count is capped, and time must not run backwards") {
    WakeConfig cfg;
    cfg.max_patches = 50;
    WakeTrail capped(cfg);
    cruise(capped, 8.0, 60.0);
    CHECK(capped.patch_count() <= 50);

    WakeConfig quick;
    quick.foam_seconds = 2.0;
    WakeTrail wake(quick);
    cruise(wake, 6.0, 10.0);
    CHECK(wake.patch_count() > 0);
    for (double t = 10.0; t < 40.0; t += 0.5) wake.update(t, 1000.0, 0.0, 0.0);  // moored far away
    CHECK(wake.patch_count() == 0);

    WakeTrail w;
    w.update(5.0, 0.0, 0.0, 1.0);
    CHECK(throws([&] { w.update(4.0, 0.0, 0.0, 1.0); }));
    CHECK(throws([&] { w.update(6.0, std::nan(""), 0.0, 1.0); }));
    WakeConfig bad;
    bad.spawn_interval = 0.0;
    CHECK(throws([&] { WakeTrail b(bad); }));
}
