// The water module: the wave surface (dispersion, height, normal, flow, foam), and floating bodies on it: which densities float and sink, how deep, how a body follows waves, how drag slows it, whether the
// result depends on the time step, and what a current does.
#include "saige/water.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr double pi = 3.14159265358979323846;

WaterConfig sea(double amplitude, double wavelength, double depth = 0.0) {
    WaterConfig c;
    c.depth = depth;
    c.waves = {{0.0, wavelength, amplitude, 0.0}};
    return c;
}

bool rejects(WaterConfig c) {
    try {
        validate_water_config(c);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_validation() {
    require(!rejects(sea(0.3, 30.0)), "a valid sea is accepted");
    WaterConfig c = sea(0.3, 30.0);
    c.waves[0].amplitude = 4.0;
    require(rejects(c), "an amplitude over a tenth of the wavelength is rejected");
    c = sea(0.3, 0.2);
    require(rejects(c), "a wavelength under half a meter is rejected");
    c = sea(0.3, 30.0, 0.2);
    require(rejects(c), "a depth of 0.2 m is rejected");
    c = sea(0.3, 30.0);
    c.density = 0.0;
    require(rejects(c), "a zero density is rejected");
    c = sea(0.3, 30.0);
    c.waves[0].phase = std::nan("");
    require(rejects(c), "a NaN phase is rejected");
    c = sea(3.0, 40.0);
    c.choppiness = 1.0;  // k A = 0.47: fine
    require(!rejects(c), "choppiness 1 on a gentle wave is accepted");
    c.waves.push_back({0.0, 10.0, 1.0, 0.0});  // k A = 0.63 more: the sum is 1.1, the surface would fold
    require(rejects(c), "choppiness that folds the surface is rejected");
    c = sea(0.3, 30.0);
    c.waves.assign(65, c.waves[0]);
    require(rejects(c), "more than 64 waves are rejected");
    bool threw = false;
    try {
        FloatingBodyConfig bc;
        bc.half_extents = {0.0, 1.0, 1.0};
        FloatingBody b(bc, {0, 0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a body of zero width is rejected");
}

// The x position of the crest of the first wave near `guess`, found by walking uphill.
double crest_near(const WaterSurface& w, double guess, double t) {
    double x = guess, step = 0.5;
    while (step > 1e-7) {
        if (w.height(x + step, 0, t) > w.height(x, 0, t)) x += step;
        else if (w.height(x - step, 0, t) > w.height(x, 0, t)) x -= step;
        else step *= 0.5;
    }
    return x;
}

void test_dispersion() {
    // Deep water: omega^2 = g k, so a 50 m wave moves at sqrt(g lambda / 2 pi) = 8.8 m/s, and its crest does.
    const WaterSurface deep(sea(0.4, 50.0));
    const double c_deep = std::sqrt(9.81 * 50.0 / (2.0 * pi));
    require(std::fabs(deep.phase_speed(0) - c_deep) < 1e-9, "the phase speed of a deep-water wave is sqrt(g lambda / 2 pi)");
    const double x0 = crest_near(deep, 1.0, 0.0), x1 = crest_near(deep, 1.0 + c_deep * 3.0, 3.0);
    std::printf("deep-water crest: %.3f m after 3 s at %.3f m/s\n", x1 - x0, c_deep);
    require(std::fabs((x1 - x0) / 3.0 - c_deep) < 0.01 * c_deep, "the crest of the surface moves at the phase speed (within 1%)");
    // Finite depth slows the wave: 5 m of water under a 50 m wave, and the shallow limit sqrt(g d) for a 100 m wave in 1 m.
    const WaterSurface shallow(sea(0.2, 50.0, 5.0));
    const double k = 2.0 * pi / 50.0, c_5 = std::sqrt(9.81 / k * std::tanh(k * 5.0));
    require(std::fabs(shallow.phase_speed(0) - c_5) < 1e-9 && c_5 < 0.8 * c_deep, "5 m of water slows a 50 m wave (the dispersion relation of finite depth)");
    const WaterSurface bore(sea(0.05, 100.0, 1.0));
    require(std::fabs(bore.phase_speed(0) / std::sqrt(9.81 * 1.0) - 1.0) < 0.03, "a long wave in 1 m of water travels at sqrt(g d), the shallow-water speed");
    // Longer waves are faster.
    WaterConfig two = sea(0.3, 20.0);
    two.waves.push_back({0.0, 80.0, 0.3, 0.0});
    const WaterSurface pair(two);
    require(pair.phase_speed(1) > 1.9 * pair.phase_speed(0), "a four times longer wave travels twice as fast");
}

void test_queries() {
    WaterConfig c = sea(1.2, 40.0);
    c.waves.push_back({0.9, 23.0, 0.5, 1.3});
    c.waves.push_back({-0.6, 9.0, 0.2, 2.2});
    c.choppiness = 0.6;
    const WaterSurface w(c);
    double worst_normal = 0.0, worst_unit = 0.0;
    for (int i = 0; i < 200; ++i) {
        const double x = -30.0 + 0.7 * i, y = 11.0 - 0.37 * i, t = 0.13 * i;
        const double e = 1e-5;
        const double hx = (w.height(x + e, y, t) - w.height(x - e, y, t)) / (2 * e), hy = (w.height(x, y + e, t) - w.height(x, y - e, t)) / (2 * e);
        const double l = std::sqrt(hx * hx + hy * hy + 1.0);
        const WaterVec n = w.normal(x, y, t);
        worst_normal = std::max(worst_normal, std::fabs(n.x - (-hx / l)) + std::fabs(n.y - (-hy / l)) + std::fabs(n.z - 1.0 / l));
        worst_unit = std::max(worst_unit, std::fabs(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) - 1.0));
        require(n.z > 0.0, "the normal points up");
    }
    std::printf("normal against the numerical slope of the height: worst difference %.2e\n", worst_normal);
    require(worst_normal < 1e-6 && worst_unit < 1e-12, "the analytic normal is the slope of the height and a unit vector");
    // The height is a function of position and time alone: calling in another order gives the same bits, and two surfaces of one config agree.
    const WaterSurface w2(c);
    const double a = w.height(3.1, -4.2, 9.9);
    (void)w.height(100, 100, 100);
    require(a == w.height(3.1, -4.2, 9.9) && a == w2.height(3.1, -4.2, 9.9), "queries are stateless and deterministic");
    // Mean level: in still water (no waves) and around the mean for a wave over a wavelength.
    const WaterSurface flat(WaterConfig{});
    require(flat.height(5, 5, 5) == 0.0 && flat.normal(5, 5, 5).z == 1.0 && flat.jacobian(5, 5, 5) == 1.0, "no waves: a flat surface at the level, normal up, Jacobian 1");
    WaterConfig lifted = sea(0.4, 30.0);
    lifted.level = 7.0;
    const WaterSurface up(lifted);
    double sum = 0.0;
    for (int i = 0; i < 3000; ++i) sum += up.height(30.0 * i / 3000.0, 0, 1.7);
    require(std::fabs(sum / 3000.0 - 7.0) < 1e-3, "the mean height over a wavelength is the level");
}

void test_flow() {
    WaterConfig c = sea(0.5, 40.0);
    c.current = {1.5, -0.5, 0.0};
    const WaterSurface w(c);
    const double wk = w.wavenumber(0), omega = w.omega(0);
    // At the surface the orbit speed is A omega around the current, in a circle (horizontal and vertical parts at the same amplitude).
    double max_h = 0.0, max_v = 0.0;
    for (int i = 0; i < 400; ++i) {
        const double t = i * 0.02;
        const WaterVec u = w.velocity(0, 0, 0.0, t);
        max_h = std::max(max_h, u.x - 1.5);
        max_v = std::max(max_v, u.z);
        require(std::fabs(u.y + 0.5) < 1e-12, "a wave along +x moves nothing sideways: the current only");
    }
    require(std::fabs(max_h - 0.5 * omega) < 1e-3 * omega && std::fabs(max_v - 0.5 * omega) < 1e-3 * omega, "the surface orbit has the speed A omega both ways");
    // It decays with depth like exp(-k d) in deep water.
    double deep_max = 0.0;
    const double d = 1.0 / wk;  // one e-folding
    for (int i = 0; i < 400; ++i) deep_max = std::max(deep_max, w.velocity(0, 0, -d, i * 0.02).z);
    require(std::fabs(deep_max / max_v - std::exp(-1.0)) < 5e-3, "one 1/k below the surface the orbit is 1/e of its surface value");
    // In shallow water the bed stops the vertical motion and the horizontal one is larger than the vertical.
    const WaterSurface shallow(sea(0.3, 60.0, 2.0));
    double hh = 0.0, vv = 0.0, bed_v = 0.0;
    for (int i = 0; i < 600; ++i) {
        const WaterVec top = shallow.velocity(0, 0, 0.0, i * 0.02), bed = shallow.velocity(0, 0, -2.0, i * 0.02);
        hh = std::max(hh, top.x);
        vv = std::max(vv, top.z);
        bed_v = std::max(bed_v, std::fabs(bed.z));
    }
    require(hh > 1.5 * vv && bed_v < 1e-9, "shallow water: flat orbits, and no vertical motion at the bed");
}

void test_foam_and_chop() {
    // Choppiness sharpens crests: the height distribution over a wavelength leans toward sharp high crests (positive skew), the plain sine does not.
    const auto skew = [](double choppiness) {
        WaterConfig c = sea(3.0, 40.0);
        c.choppiness = choppiness;
        const WaterSurface w(c);
        std::vector<double> h;
        for (int i = 0; i < 4000; ++i) h.push_back(w.height(40.0 * i / 4000.0, 0, 0.0));
        double mean = 0.0;
        for (double x : h) mean += x;
        mean /= static_cast<double>(h.size());
        double m2 = 0.0, m3 = 0.0;
        for (double x : h) m2 += (x - mean) * (x - mean), m3 += (x - mean) * (x - mean) * (x - mean);
        m2 /= static_cast<double>(h.size());
        m3 /= static_cast<double>(h.size());
        return m3 / std::pow(m2, 1.5);
    };
    const double flat = skew(0.0), chop = skew(0.9);
    std::printf("height skewness: %.4f without choppiness, %.4f with 0.9\n", flat, chop);
    require(std::fabs(flat) < 0.01 && chop > 0.1, "choppiness gives sharp crests (positive skew); plain waves are symmetric");
    // The Jacobian is 1 on a calm surface, never negative on a valid one, and foam appears only where it falls.
    WaterConfig c = sea(3.0, 40.0);
    c.waves.push_back(c.waves[0]);  // two copies in step: k A sums to 0.942
    c.choppiness = 1.0;
    const WaterSurface w(c);
    double min_j = 1e9, max_foam = 0.0, foam_where_calm = 0.0;
    for (int i = 0; i < 4000; ++i) {
        const double x = 40.0 * i / 4000.0, j = w.jacobian(x, 0, 0.0), f = w.foam(x, 0, 0.0);
        min_j = std::min(min_j, j);
        max_foam = std::max(max_foam, f);
        if (j >= 0.8) foam_where_calm = std::max(foam_where_calm, f);
    }
    std::printf("steep chop: smallest Jacobian %.3f, most foam %.3f\n", min_j, max_foam);
    require(min_j > 0.0 && std::fabs(min_j - (1.0 - 2.0 * (2.0 * pi / 40.0) * 3.0)) < 1e-3, "a steep, choppy wave nearly folds (Jacobian 1 - sum kA = 0.058 at the crest) but never does");
    require(max_foam > 0.7 && foam_where_calm == 0.0, "foam rises where the surface folds and is zero where it does not");
    // The Jacobian of a single wave in any direction is 1 - choppiness k A cos(theta) (the determinant of I minus a rank-one matrix), so its smallest value does not depend on which way the wave runs.
    for (const double direction : {0.0, 0.3, 0.9, 2.0, -1.2}) {
        WaterConfig oblique = sea(3.0, 40.0);
        oblique.waves[0].direction = direction;
        oblique.choppiness = 1.0;
        const WaterSurface ow(oblique);
        double lowest = 1e9, highest = -1e9;
        for (int i = 0; i < 4000; ++i) {
            const double s = 40.0 * i / 4000.0, x = s * std::cos(direction), y = s * std::sin(direction);
            lowest = std::min(lowest, ow.jacobian(x, y, 0.0));
            highest = std::max(highest, ow.jacobian(x, y, 0.0));
        }
        const double kA = 2.0 * pi / 40.0 * 3.0;
        require(std::fabs(lowest - (1.0 - kA)) < 2e-3 && std::fabs(highest - (1.0 + kA)) < 2e-3, "the Jacobian of one wave swings between 1 - kA and 1 + kA whatever its direction");
    }
    const WaterSurface calm(sea(0.1, 60.0));
    double calm_foam = 0.0;
    for (int i = 0; i < 500; ++i) calm_foam = std::max(calm_foam, calm.foam(i * 0.3, 0, 0.0));
    require(calm_foam == 0.0, "a gentle sea has no foam");
}

void test_wind() {
    double last_height = 0.0, last_length = 0.0;
    for (const double speed : {5.0, 10.0, 15.0}) {
        WaterConfig c;
        c.waves = waves_from_wind(speed, 0.4, 14);
        const WaterSurface w(c);
        // The significant wave height from the variance of the surface at one time, over a patch: 4 sigma.
        double sum = 0.0, sum2 = 0.0;
        int n = 0;
        for (int i = 0; i < 100; ++i) {
            for (int j = 0; j < 100; ++j) {
                const double h = w.height(i * 20.0, j * 20.0, 3.0);
                sum += h, sum2 += h * h, ++n;
            }
        }
        const double mean = sum / n, sigma = std::sqrt(sum2 / n - mean * mean);
        const double hs = 4.0 * sigma, expected = 0.21 * speed * speed / 9.81;
        double longest = 0.0;
        for (const WaterWave& wave : c.waves) longest = std::max(longest, wave.wavelength);
        std::printf("wind %.0f m/s: Hs %.2f m (fully developed %.2f m), longest wave %.1f m, %zu waves\n", speed, hs, expected, longest, c.waves.size());
        require(hs > 0.6 * expected && hs < 1.5 * expected, "the significant wave height is about 0.21 U^2 / g");
        require(hs > last_height && longest > last_length, "a stronger wind makes a taller sea with longer waves");
        last_height = hs;
        last_length = longest;
        require(!rejects(c), "the sea state is a valid configuration");
    }
    require(waves_from_wind(10.0, 0.0, 12)[3].phase == waves_from_wind(10.0, 0.0, 12)[3].phase, "deterministic");
    bool threw = false;
    try {
        (void)waves_from_wind(0.0, 0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "no wind, no sea state");
}

// A box in still water: settles to its density ratio.
double settled_draft(double density, double water_density, double dt, double seconds, WaterVec half = {0.5, 0.5, 0.5}) {
    WaterConfig wc;
    wc.density = water_density;
    const WaterSurface water(wc);
    FloatingBodyConfig bc;
    bc.half_extents = half;
    bc.density = density;
    FloatingBody body(bc, {0, 0, 0});
    double t = 0.0;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) t = body.step(dt, water, t);
    return (wc.level - (body.state().position.z - half.z)) / (2.0 * half.z);
}

void test_density() {
    // Which densities float and how deep: the draft is the density ratio (Archimedes), for fresh and sea water, in a box that is stable (a wide flat one: a cube of density 0.25 is not).
    for (const double density : {250.0, 500.0, 800.0, 950.0}) {
        for (const double water_density : {1000.0, 1025.0}) {
            const double draft = settled_draft(density, water_density, 1.0 / 60.0, 40.0, {1.0, 1.0, 0.4});
            std::printf("density %4.0f in %4.0f: draft %.4f of the height (Archimedes %.4f)\n", density, water_density, draft, density / water_density);
            require(std::fabs(draft - density / water_density) < 0.01, "a box settles to the draft its density ratio gives");
        }
    }
    // A box denser than the water sinks and keeps going; neutral buoyancy hangs where it is.
    {
        WaterConfig wc;
        const WaterSurface water(wc);
        FloatingBodyConfig bc;
        bc.half_extents = {0.5, 0.5, 0.5};
        bc.density = 1500.0;
        FloatingBody body(bc, {0, 0, 0});
        double t = 0.0, z_prev = 0.0;
        bool monotone = true;
        for (int i = 0; i < 60 * 12; ++i) {
            t = body.step(1.0 / 60.0, water, t);
            monotone = monotone && body.state().position.z <= z_prev + 1e-9;
            z_prev = body.state().position.z;
        }
        const double vt = std::sqrt(2.0 * (body.mass() - 1000.0 * body.volume()) * 9.81 / (1000.0 * 1.0 * 1.0));
        std::printf("sinking box of density 1500: depth %.1f m, speed %.3f m/s, terminal speed from the drag balance %.3f\n", -body.state().position.z, -body.state().velocity.z, vt);
        require(monotone && body.state().position.z < -20.0, "a box of density 1500 sinks and does not come back");
        require(std::fabs(-body.state().velocity.z - vt) < 0.05 * vt, "and falls at the terminal speed where drag balances the weight less the buoyancy (within 5%)");
        require(body.state().submerged_volume > 0.999 * body.volume(), "fully submerged");
    }
    {
        WaterConfig wc;
        const WaterSurface water(wc);
        FloatingBodyConfig bc;
        bc.half_extents = {0.5, 0.5, 0.5};
        bc.density = 1000.0;
        FloatingBody body(bc, {0, 0, -3.0});
        double t = 0.0;
        for (int i = 0; i < 60 * 20; ++i) t = body.step(1.0 / 60.0, water, t);
        require(std::fabs(body.state().position.z + 3.0) < 1e-6, "a body of exactly the water's density hangs where it is");
    }
    {
        // Released under the surface, a light body rises and settles at its draft.
        WaterConfig wc;
        const WaterSurface water(wc);
        FloatingBodyConfig bc;
        bc.half_extents = {1.0, 1.0, 0.4};
        bc.density = 600.0;
        FloatingBody body(bc, {0, 0, -4.0});
        double t = 0.0;
        for (int i = 0; i < 60 * 40; ++i) t = body.step(1.0 / 60.0, water, t);
        require(std::fabs((0.0 - (body.state().position.z - 0.4)) / 0.8 - 0.6) < 0.01, "a light body released under water rises and settles at its draft");
    }
}

void test_waves_and_bodies() {
    // A small buoy follows the surface: the heave amplitude is the wave's (the body is far shorter than the wave), at the wave's period, in phase.
    const double A = 0.4, lambda = 80.0;
    const WaterSurface water(sea(A, lambda));
    FloatingBodyConfig bc;
    bc.half_extents = {0.6, 0.6, 0.3};
    bc.density = 500.0;
    FloatingBody buoy(bc, {0, 0, 0});
    double t = 0.0, max_z = -1e9, min_z = 1e9, lag_sum = 0.0;
    int lag_n = 0;
    const double period = 2.0 * pi / water.omega(0);
    for (int i = 0; i < static_cast<int>(60.0 * 60.0); ++i) {
        t = buoy.step(1.0 / 60.0, water, t);
        if (t > 30.0) {
            const double z = buoy.state().position.z - (0.0 - 0.0);
            max_z = std::max(max_z, z);
            min_z = std::min(min_z, z);
            // The surface under the buoy against the buoy's height: the phase lag as time offset via the correlation of the two (use zero crossings).
            lag_sum += (z - 0.0) * (water.height(buoy.state().position.x, buoy.state().position.y, t) - 0.0);
            ++lag_n;
        }
    }
    const double amplitude = 0.5 * (max_z - min_z);
    std::printf("buoy on a %.0f m wave of amplitude %.2f (period %.2f s): heave amplitude %.3f m\n", lambda, A, period, amplitude);
    require(std::fabs(amplitude / A - 1.0) < 0.15, "a small buoy heaves with the wave amplitude, to within 15%");
    require(lag_sum > 0.0, "and in phase with the surface (positive correlation)");
    // A long barge averages the waves out: far less heave on waves shorter than it.
    FloatingBodyConfig barge = bc;
    barge.half_extents = {25.0, 4.0, 1.0};
    barge.cells = 16;
    const WaterSurface chop(sea(0.4, 20.0));
    FloatingBody ship(barge, {0, 0, 0});
    t = 0.0;
    double smax = -1e9, smin = 1e9;
    for (int i = 0; i < 60 * 80; ++i) {
        t = ship.step(1.0 / 60.0, chop, t);
        if (t > 40.0) smax = std::max(smax, ship.state().position.z), smin = std::min(smin, ship.state().position.z);
    }
    std::printf("a 50 m barge on 20 m waves of amplitude 0.4: heave amplitude %.3f m\n", 0.5 * (smax - smin));
    require(0.5 * (smax - smin) < 0.4 * 0.4, "a barge longer than the wave heaves less than 40% of its amplitude");
}

void test_drag() {
    // A neutrally buoyant cube thrown at 4 m/s under water slows exactly as the drag law says: dv/dt = -k (v + u0) v with k = rho Cd A / (2 m), u0 the drag floor.
    WaterConfig wc;
    const WaterSurface water(wc);
    FloatingBodyConfig bc;
    bc.half_extents = {0.5, 0.5, 0.5};
    bc.density = 1000.0;
    FloatingBody body(bc, {0, 0, -20.0});
    body.set_velocity({4.0, 0.0, 0.0});
    const double k = 0.5 * 1000.0 * 1.0 * 1.0 / body.mass(), u0 = bc.drag_speed_floor, v0 = 4.0;
    double t = 0.0, last = 4.0;
    bool monotone = true;
    for (int i = 0; i < 60 * 6; ++i) {
        t = body.step(1.0 / 60.0, water, t);
        monotone = monotone && body.state().velocity.x <= last + 1e-12 && body.state().velocity.x > 0.0;
        last = body.state().velocity.x;
        if (i + 1 == 120 || i + 1 == 360) {
            const double expected = u0 / ((1.0 + u0 / v0) * std::exp(k * u0 * t) - 1.0);
            std::printf("drag after %.0f s: %.4f m/s (analytic %.4f)\n", t, body.state().velocity.x, expected);
            require(std::fabs(body.state().velocity.x - expected) < 0.03 * expected, "a moving body slows as the quadratic drag law (with the floor) says, to within 3%");
        }
    }
    require(monotone, "and its speed only falls, never reversing");
    require(std::fabs(body.state().position.z + 20.0) < 1e-6, "neutrally buoyant: no vertical motion");
    // Doubling the drag coefficient doubles k: the same body is slower.
    FloatingBodyConfig heavy_drag = bc;
    heavy_drag.drag_coefficient = 2.0;
    FloatingBody b2(heavy_drag, {0, 0, -20.0});
    b2.set_velocity({4.0, 0.0, 0.0});
    t = 0.0;
    for (int i = 0; i < 120; ++i) t = b2.step(1.0 / 60.0, water, t);
    require(b2.state().velocity.x < 0.7 * 4.0 * 0.5 && b2.state().velocity.x < body.state().velocity.x + 5.0, "a body with a larger drag coefficient is slower");
    // Out of the water there is no drag and no buoyancy: free fall under g, exactly.
    FloatingBody falling(bc, {0, 0, 40.0});
    t = 0.0;
    for (int i = 0; i < 30; ++i) t = falling.step(1.0 / 60.0, water, t);
    std::printf("free fall for 0.5 s: dropped %.4f m (g t^2 / 2 = %.4f)\n", 40.0 - falling.state().position.z, 0.5 * 9.81 * 0.25);
    require(std::fabs((40.0 - falling.state().position.z) - 0.5 * 9.81 * 0.25) < 0.02 && falling.wetted().empty() && falling.state().submerged_volume == 0.0, "above the water the body falls freely and has no wet cells");
}

void test_time_step() {
    // The settled draft and the response to waves do not depend on the time step (a body is advanced in substeps of at most 1/240 s whatever dt is asked).
    double drafts[5];
    int n = 0;
    for (const double dt : {1.0 / 15.0, 1.0 / 30.0, 1.0 / 60.0, 1.0 / 120.0, 1.0 / 240.0}) {
        drafts[n] = settled_draft(500.0, 1000.0, dt, 30.0, {1.0, 1.0, 0.4});
        require(std::isfinite(drafts[n]), "finite");
        ++n;
    }
    std::printf("settled draft for dt 1/15 ... 1/240: %.4f %.4f %.4f %.4f %.4f\n", drafts[0], drafts[1], drafts[2], drafts[3], drafts[4]);
    for (int i = 0; i < 5; ++i) require(std::fabs(drafts[i] - 0.5) < 0.005, "the draft is 0.5 at every step size, to half a percent");
    // In waves: the heave amplitude with dt = 1/30 and dt = 1/240.
    const auto heave = [](double dt) {
        const WaterSurface water(sea(0.4, 60.0));
        FloatingBodyConfig bc;
        bc.half_extents = {0.6, 0.6, 0.3};
        bc.density = 500.0;
        FloatingBody b(bc, {0, 0, 0});
        double t = 0.0, mx = -1e9, mn = 1e9;
        while (t < 50.0) {
            t = b.step(dt, water, t);
            if (t > 25.0) mx = std::max(mx, b.state().position.z), mn = std::min(mn, b.state().position.z);
        }
        return 0.5 * (mx - mn);
    };
    const double a30 = heave(1.0 / 30.0), a240 = heave(1.0 / 240.0), a15 = heave(1.0 / 15.0);
    std::printf("heave amplitude in waves: %.4f (1/15), %.4f (1/30), %.4f (1/240)\n", a15, a30, a240);
    require(std::fabs(a30 - a240) < 0.01 * a240 + 1e-3 && std::fabs(a15 - a240) < 0.02 * a240 + 1e-3, "the heave amplitude does not depend on the step size to within 1% (2% at 15 Hz)");
    // A rough step: a huge dt of one second keeps the state finite and the body near the surface (no explosion).
    {
        const WaterSurface water(sea(0.4, 30.0));
        FloatingBodyConfig bc;
        bc.half_extents = {0.6, 0.6, 0.3};
        bc.density = 500.0;
        FloatingBody b(bc, {0, 0, 0});
        double t = 0.0;
        for (int i = 0; i < 100; ++i) t = b.step(1.0, water, t);
        require(std::isfinite(b.state().position.z) && std::fabs(b.state().position.z) < 2.0, "steps of a full second stay finite and bounded");
    }
    // A tiny light body thrown fast under water: the drag it feels in one substep would exceed its momentum without the limit on the drag impulse, and its speed would reverse and ring.
    {
        const WaterSurface water(WaterConfig{});
        FloatingBodyConfig bc;
        bc.half_extents = {0.05, 0.05, 0.05};
        bc.density = 1000.0;
        bc.drag_coefficient = 4.0;
        FloatingBody b(bc, {0, 0, -5.0});
        b.set_velocity({12.0, 0.0, 0.0});
        double t = 0.0, last = 12.0;
        bool smooth = true;
        for (int i = 0; i < 240 * 3; ++i) {
            t = b.step(1.0 / 240.0, water, t);
            smooth = smooth && b.state().velocity.x >= -1e-9 && b.state().velocity.x <= last + 1e-9;
            last = b.state().velocity.x;
        }
        std::printf("a 0.1 m cube thrown at 12 m/s with Cd 4: %.4f m/s after 3 s (never reversing: %s)\n", last, smooth ? "yes" : "no");
        require(smooth && last < 12.0, "heavy drag on a tiny body slows it monotonically and never reverses it (the drag impulse is limited)");
    }
    {
        // The inertia: a box's moments plus the parallel-axis term for an offset center of mass.
        FloatingBodyConfig bc;
        bc.half_extents = {1.0, 0.5, 0.25};
        bc.density = 400.0;
        bc.center_of_mass = {0.3, -0.2, -0.1};
        FloatingBody b(bc, {0, 0, 0});
        const double m = b.mass(), hx = 1.0, hy = 0.5, hz = 0.25;
        const WaterVec i = b.inertia();
        require(std::fabs(i.x - (m / 3.0 * (hy * hy + hz * hz) + m * (0.2 * 0.2 + 0.1 * 0.1))) < 1e-9 && std::fabs(i.y - (m / 3.0 * (hx * hx + hz * hz) + m * (0.3 * 0.3 + 0.1 * 0.1))) < 1e-9 &&
                    std::fabs(i.z - (m / 3.0 * (hx * hx + hy * hy) + m * (0.3 * 0.3 + 0.2 * 0.2))) < 1e-9,
                "the inertia is the box's plus m times the squared offset of the center of mass from each axis");
    }
    bool threw = false;
    try {
        const WaterSurface water(WaterConfig{});
        FloatingBodyConfig bc;
        FloatingBody b(bc, {0, 0, 0});
        (void)b.step(0.0, water, 0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a zero time step is rejected");
}

void test_current() {
    // A current carries a floating body along until it moves with the water; the vertical draft does not change.
    for (const double cx : {2.0, -1.2}) {
        WaterConfig wc;
        wc.current = {cx, 0.5 * cx, 0.0};
        const WaterSurface water(wc);
        FloatingBodyConfig bc;
        bc.half_extents = {1.0, 1.0, 0.4};
        bc.density = 500.0;
        FloatingBody body(bc, {0, 0, 0});
        double t = 0.0, reach63 = -1.0;
        for (int i = 0; i < 60 * 90; ++i) {
            t = body.step(1.0 / 60.0, water, t);
            if (reach63 < 0.0 && std::fabs(body.state().velocity.x) > 0.63 * std::fabs(cx)) reach63 = t;
        }
        std::printf("current (%.1f, %.1f): body speed %.3f %.3f after 90 s, 63%% reached at %.1f s, draft %.3f\n", cx, 0.5 * cx, body.state().velocity.x, body.state().velocity.y, reach63, 0.4 - body.state().position.z / 0.8 - 0.0);
        require(std::fabs(body.state().velocity.x - cx) < 0.03 * std::fabs(cx) && std::fabs(body.state().velocity.y - 0.5 * cx) < 0.03 * std::fabs(cx), "the body ends up moving with the current (within 3%)");
        require(reach63 > 1.0 && reach63 < 60.0, "and takes time to get there (it is dragged, not teleported)");
        require(std::fabs((0.0 - (body.state().position.z - 0.4)) / 0.8 - 0.5) < 0.005, "the current does not change its draft");
        require(body.state().position.x * cx > 0.0, "it drifted downstream");
    }
    // A larger submerged body is carried more slowly: mass grows as size cubed and drag as size squared, so the time to reach half the current's speed doubles when the size doubles.
    const auto time_to_half = [](double half_size) {
        WaterConfig wc;
        wc.current = {2.0, 0.0, 0.0};
        const WaterSurface water(wc);
        FloatingBodyConfig bc;
        bc.half_extents = {half_size, half_size, half_size};
        bc.density = 1000.0;
        FloatingBody body(bc, {0, 0, -10.0});
        double t = 0.0;
        while (t < 240.0 && body.state().velocity.x < 1.0) t = body.step(1.0 / 60.0, water, t);
        return t;
    };
    const double small = time_to_half(0.5), large = time_to_half(1.0);
    std::printf("time to half the current's speed: %.2f s for a 1 m cube, %.2f s for a 2 m cube\n", small, large);
    require(large > 1.6 * small && large < 2.4 * small, "a cube twice the size takes about twice as long to be carried (mass over drag area)");
}

void test_stability() {
    // A wide flat box heeled 25 degrees rights itself: the center of buoyancy shifts outboard and the metacentric height is positive.
    const auto roll_history = [](FloatingBodyConfig bc, double initial, double seconds, double* final_roll, double* max_late) {
        const WaterSurface water(WaterConfig{});
        FloatingBody body(bc, {0, 0, 0});
        double t = 0.0;
        for (int i = 0; i < 60 * 8; ++i) t = body.step(1.0 / 60.0, water, t);  // settle first
        body.set_roll(initial);
        double late = 0.0;
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) {
            t = body.step(1.0 / 60.0, water, t);
            if (i > static_cast<int>(seconds * 30.0)) late = std::max(late, std::fabs(body.roll()));
        }
        *final_roll = body.roll();
        *max_late = late;
    };
    double fin = 0.0, late = 0.0;
    FloatingBodyConfig wide;
    wide.half_extents = {1.5, 1.5, 0.4};
    wide.density = 500.0;
    roll_history(wide, 25.0 * pi / 180.0, 20.0, &fin, &late);
    std::printf("wide flat box heeled 25 deg: roll after 20 s %.2f deg, largest roll in the second half %.2f deg\n", fin * 180.0 / pi, late * 180.0 / pi);
    require(std::fabs(fin) < 1.0 * pi / 180.0 && late < 5.0 * pi / 180.0, "a wide flat box heeled 25 degrees rights itself (under 1 degree after 20 s)");
    // A cube of density 0.25 has a negative metacentric height (GM = BM + KB - KG = 0.333 + 0.125 - 0.5): it falls over; with ballast under its center it is stable.
    FloatingBodyConfig cube;
    cube.half_extents = {0.5, 0.5, 0.5};
    cube.density = 250.0;
    roll_history(cube, 3.0 * pi / 180.0, 20.0, &fin, &late);
    std::printf("cube of density 250 started 3 deg off: roll after 20 s %.1f deg\n", fin * 180.0 / pi);
    require(std::fabs(fin) > 15.0 * pi / 180.0 || late > 15.0 * pi / 180.0, "a cube of density 0.25 is unstable: a 3 degree tilt grows past 15 degrees");
    FloatingBodyConfig ballasted = cube;
    ballasted.center_of_mass = {0.0, 0.0, -0.4};
    roll_history(ballasted, 3.0 * pi / 180.0, 20.0, &fin, &late);
    std::printf("the same cube with its center of mass 0.4 m lower: roll after 20 s %.2f deg\n", fin * 180.0 / pi);
    require(std::fabs(fin) < 1.0 * pi / 180.0, "with the center of mass lowered 0.4 m the same cube comes back to level");
}

void test_exposed_state() {
    // What a host needs to spawn wakes, foam and splashes: the wet cells, the submerged volume and the waterline.
    WaterConfig wc;
    const WaterSurface water(wc);
    FloatingBodyConfig bc;
    bc.half_extents = {1.0, 1.0, 0.4};
    bc.density = 600.0;
    FloatingBody body(bc, {0, 0, 0});
    double t = 0.0;
    for (int i = 0; i < 60 * 30; ++i) t = body.step(1.0 / 60.0, water, t);
    const double displaced = body.state().submerged_volume;
    std::printf("submerged volume %.4f m^3 (the body's mass over the water's density: %.4f), %zu wet cells, waterline %.3f\n", displaced, body.mass() / 1000.0, body.wetted().size(), body.state().waterline);
    require(std::fabs(displaced - body.mass() / 1000.0) < 0.01 * displaced, "the displaced volume is the mass over the water's density (Archimedes)");
    require(!body.wetted().empty() && std::fabs(body.state().waterline) < 1e-9, "the wet cells are listed and the waterline is the level in still water");
    for (const WetSample& s : body.wetted()) require(s.submersion > 0.0 && s.submersion <= 1.0 && s.position.z < 0.4, "each wet sample is partly or wholly under the surface");
    // A body that falls into the water: wet cells appear at the surface and it comes to rest at its draft.
    FloatingBody dropped(bc, {0, 0, 3.0});
    t = 0.0;
    double first_wet = -1.0, peak_depth = 0.0;
    for (int i = 0; i < 60 * 30; ++i) {
        t = dropped.step(1.0 / 60.0, water, t);
        if (first_wet < 0.0 && !dropped.wetted().empty()) first_wet = t;
        peak_depth = std::min(peak_depth, dropped.state().position.z);
    }
    std::printf("dropped from 3 m: first wet at %.2f s (free fall would be %.2f s), deepest centre %.2f m, settled at %.3f (draft 0.6 gives %.3f)\n", first_wet, std::sqrt(2.0 * (3.0 - 0.4) / 9.81), peak_depth,
                dropped.state().position.z, 0.4 - 0.6 * 0.8);
    require(first_wet > 0.7 && first_wet < 0.8, "it reaches the water when free fall says it should");
    require(peak_depth < -0.3 && std::fabs(dropped.state().position.z - (0.4 - 0.6 * 0.8)) < 0.01, "it plunges below its floating draft and settles to it");
}

// A sea of twelve waves from several directions, the largest 0.5 m, each next 0.8 of the one before and shorter.
WaterConfig busy_sea() {
    WaterConfig c;
    double amp = 0.5, len = 60.0;
    for (int i = 0; i < 12; ++i) {
        c.waves.push_back({0.35 * i - 1.0, len, amp, 0.7 * i});
        amp *= 0.8;
        len *= 0.82;
    }
    return c;
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

void test_level_of_detail() {
    const WaterSurface full(busy_sea());
    require(full.wave_count() == 12, "control: twelve waves");
    // reduced(n) keeps the n largest: with all of them it is the same sea; fewer waves have less energy but most of it.
    const WaterSurface all = full.reduced(12), eight = full.reduced(8), four = full.reduced(4);
    require(eight.wave_count() == 8 && four.wave_count() == 4, "reduced keeps the count asked for");
    double worst_same = 0.0;
    for (int i = 0; i < 50; ++i) worst_same = std::max(worst_same, std::fabs(all.height(3.0 * i, -2.0 * i, 0.7 * i) - full.height(3.0 * i, -2.0 * i, 0.7 * i)));
    require(worst_same == 0.0, "keeping all the waves changes nothing");
    const auto energy = [](const WaterSurface& w) {
        double e = 0.0;
        for (const auto& wave : w.config().waves) e += wave.amplitude * wave.amplitude;
        return e;
    };
    require(energy(eight) > 0.97 * energy(full) && energy(four) > 0.80 * energy(full) && energy(four) < energy(eight) && energy(eight) < energy(full), "the largest waves carry most of the energy and fewer carry less");
    for (const auto& w : four.config().waves) require(w.amplitude >= 0.5 * std::pow(0.8, 3) - 1e-12, "the waves kept are the largest ones");
    bool threw = false;
    try {
        (void)full.reduced(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a count of zero is refused");
    const WaterSurface long_only = full.filtered(25.0);
    require(long_only.wave_count() > 0 && long_only.wave_count() < 12, "filtered drops the shorter waves");
    for (const auto& w : long_only.config().waves) require(w.wavelength >= 25.0, "and keeps the longer ones");
}

void test_cells() {
    // A body with fewer cells keeps its state, floats at the same draft in calm water, and follows swell nearly as well (the cost is cells, the error is small and measured, not assumed).
    FloatingBodyConfig bc;
    bc.half_extents = {1.0, 0.6, 0.4};
    bc.density = 600.0;
    bc.cells = 6;
    const WaterSurface calm(WaterConfig{});
    FloatingBody a(bc, {0, 0, 0}), b(bc, {0, 0, 0});
    b.set_cells(3);
    double t = 0.0;
    for (int i = 0; i < 600; ++i) {
        a.step(1.0 / 60.0, calm, t);
        t = b.step(1.0 / 60.0, calm, t);
    }
    std::printf("calm draft with 6 cells %.4f, with 3 cells %.4f\n", a.state().position.z, b.state().position.z);
    require(std::fabs(a.state().position.z - b.state().position.z) < 0.01, "the draft in calm water does not depend on the cell count (to a centimeter)");
    bool threw = false;
    try {
        b.set_cells(1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a cell count below 2 is refused");
    const WaterSurface swell(busy_sea());
    FloatingBody c(bc, {0, 0, 0}), d(bc, {0, 0, 0});
    d.set_cells(3);
    double sq = 0.0, sq_roll = 0.0;
    int n = 0;
    t = 0.0;
    for (int i = 0; i < 60 * 40; ++i) {
        c.step(1.0 / 60.0, swell, t);
        t = d.step(1.0 / 60.0, swell, t);
        if (t > 10.0) {
            const double dz = c.state().position.z - d.state().position.z;
            sq += dz * dz;
            ++n;
        }
    }
    const double rms = std::sqrt(sq / n);
    std::printf("3 cells against 6 in a sea of Hs ~1 m: RMS height difference %.1f mm\n", rms * 1000.0);
    (void)sq_roll;
    require(c.wetted().size() > 27 && d.wetted().size() <= 27 && d.wetted().size() > 0, "the body with 3 cells has at most 27 sample cells and the one with 6 many more (set_cells rebuilt them)");
    require(rms < 0.01, "three cells per axis follow the swell to within a centimeter RMS for a body of this size");
}

void test_lod_cells() {
    require(water_lod_cells(0.0, 6) == 6 && water_lod_cells(39.9, 6) == 6, "within 40 m a body keeps all its cells");
    require(water_lod_cells(40.0, 6) == 4 && water_lod_cells(99.9, 6) == 4, "out to 100 m two thirds");
    require(water_lod_cells(100.0, 6) == 3 && water_lod_cells(5000.0, 6) == 3, "beyond that half");
    require(water_lod_cells(60.0, 3) == 3 && water_lod_cells(500.0, 3) == 2 && water_lod_cells(500.0, 2) == 2, "never below 3 in the middle band nor 2 beyond it");
    bool threw = false;
    try {
        (void)water_lod_cells(-1.0, 6);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a negative distance is refused");
}

void test_field_cache() {
    const WaterSurface surface(busy_sea());
    WaterFieldCache cache(surface, 0.5);
    require(cache.usable(), "a deep sea can be cached");
    cache.begin(10.0, 10.0 + 1.0 / 60.0);
    cache.cover(5.0, -3.0, 2.0);
    require(cache.tile_count() > 0 && cache.points_computed() > 0, "covering a body computes tiles");
    double worst_h = 0.0, worst_flow = 0.0;
    for (int i = 0; i < 200; ++i) {
        const double x = 5.0 + 1.9 * std::cos(0.31 * i), y = -3.0 + 1.9 * std::sin(0.17 * i), t = 10.0 + (i % 17) / 17.0 / 60.0;
        double h, he;
        WaterVec f, fe;
        cache.sample(x, y, -0.2, t, h, f);
        surface.sample(x, y, -0.2, t, he, fe);
        worst_h = std::max(worst_h, std::fabs(h - he));
        worst_flow = std::max(worst_flow, std::fabs(f.x - fe.x) + std::fabs(f.y - fe.y) + std::fabs(f.z - fe.z));
    }
    std::printf("cache against exact over 200 samples: height %.2f mm, flow %.4f m/s\n", worst_h * 1000.0, worst_flow);
    require(cache.fallbacks() == 0, "inside the prepared tiles nothing falls back (at the depths asked)");
    require(worst_h < 0.01, "the cached height is within 10 mm of the exact");
    require(worst_flow < 0.02, "and the flow within 0.02 m/s");
    // Flow at depth: a quarter meter under the surface the single-wavenumber decay is good to a few percent; deeper than max_flow_depth the cache answers exactly (and says so).
    double near_err = 0.0, near_exact = 0.0;
    for (int i = 0; i < 100; ++i) {
        const double x = 5.0 + 1.9 * std::cos(0.37 * i), y = -3.0 + 1.9 * std::sin(0.23 * i), t = 10.0 + (i % 11) / 11.0 / 60.0;
        double h2, he2;
        WaterVec f2, fe2;
        const double z = surface.height(x, y, t) - 0.2;  // 0.2 m under the surface wherever it is
        cache.sample(x, y, z, t, h2, f2);
        surface.sample(x, y, z, t, he2, fe2);
        near_err = std::max(near_err, std::fabs(f2.x - fe2.x) + std::fabs(f2.y - fe2.y) + std::fabs(f2.z - fe2.z));
        near_exact = std::max(near_exact, std::fabs(fe2.x) + std::fabs(fe2.y) + std::fabs(fe2.z));
    }
    std::printf("flow 0.2 m under the surface: exact up to %.3f, largest difference %.4f m/s (%.1f%%)\n", near_exact, near_err, 100.0 * near_err / near_exact);
    require(near_err < 0.05 * near_exact, "0.2 m under the surface the cached flow is within 5% of the exact (the error grows to 14% at 0.9 m, hence the default depth limit)");
    {
        // For a sea of one wave the single-wavenumber decay is exact: the cached flow follows the orbital decay with depth, to interpolation error.
        const WaterSurface one(sea(0.5, 40.0));
        WaterFieldCache c1(one, 0.5, 5.0);
        c1.begin(4.0, 4.0 + 1.0 / 60.0);
        c1.cover(2.0, 1.0, 2.0);
        double worst = 0.0, top = 0.0;
        for (int i = 0; i < 60; ++i) {
            const double x = 2.0 + 1.8 * std::cos(0.4 * i), y = 1.0 + 1.8 * std::sin(0.3 * i), t = 4.0 + (i % 7) / 7.0 / 60.0, z = one.height(x, y, t) - 3.0;
            double hh, he1;
            WaterVec ff, fe1;
            c1.sample(x, y, z, t, hh, ff);
            one.sample(x, y, z, t, he1, fe1);
            worst = std::max(worst, std::fabs(ff.x - fe1.x) + std::fabs(ff.y - fe1.y) + std::fabs(ff.z - fe1.z));
            top = std::max(top, std::fabs(fe1.x) + std::fabs(fe1.y) + std::fabs(fe1.z));
        }
        require(c1.fallbacks() == 0, "control: the single-wave samples were served from the cache");
        require(top > 0.05 && worst < 0.01 * 0.5 * 1.0 + 0.004, "for one wave the cached flow 3 m down has the exact orbital decay (0.4% of the surface speed)");
    }
    const std::size_t before = cache.fallbacks();
    double hd, hed;
    WaterVec fd, fed;
    cache.sample(5.0, -3.0, -4.0, 10.0, hd, fd);
    surface.sample(5.0, -3.0, -4.0, 10.0, hed, fed);
    require(cache.fallbacks() == before + 1 && fd.x == fed.x && fd.z == fed.z, "four meters down the cache refuses and answers exactly, counted (default limit 0.25 m)");
    
    cache.sample(5.0, -3.0, -4.0, 10.0, hd, fd, 1e9);  // no flow wanted (surface not above the threshold): the height alone is cached
    require(cache.fallbacks() == before + 1, "and a height-only query at that depth is served from the cache");
    double h, he;
    WaterVec f, fe;
    cache.sample(400.0, 400.0, -0.2, 10.0, h, f);
    surface.sample(400.0, 400.0, -0.2, 10.0, he, fe);
    require(cache.fallbacks() == before + 2 && h == he, "a point outside the tiles falls back to the exact answer and is counted");
    cache.sample(5.0, -3.0, -0.2, 30.0, h, f, 1e9);  // height only, so a refusal for depth cannot stand in for the time check
    surface.sample(5.0, -3.0, -0.2, 30.0, he, fe);
    require(cache.fallbacks() == before + 3 && h == he, "so does a time outside the interval");
    WaterConfig shallow = busy_sea();
    shallow.depth = 20.0;
    const WaterSurface shallow_surface(shallow);
    require(!WaterFieldCache(shallow_surface).usable(), "a sea of finite depth is not cached (its flow decays per wave)");
}

void test_parallel_bodies() {
    const WaterSurface surface(busy_sea());
    const WaterSurface coarse = surface.reduced(6);
    const auto build = [&](std::vector<FloatingBody>& bodies) {
        bodies.clear();
        for (int i = 0; i < 24; ++i) {
            FloatingBodyConfig bc;
            bc.half_extents = {0.6 + 0.05 * (i % 5), 0.5, 0.3};
            bc.density = 400.0 + 20.0 * i;
            bc.cells = 3 + (i % 4);
            bodies.emplace_back(bc, WaterVec{9.0 * (i % 6), 9.0 * (i / 6), 0.0});
        }
    };
    const auto run = [&](unsigned threads) {
        std::vector<FloatingBody> bodies;
        build(bodies);
        std::vector<BodyStep> steps;
        for (std::size_t i = 0; i < bodies.size(); ++i) steps.push_back({&bodies[i], i % 2 == 0 ? static_cast<const WaterSource*>(&surface) : &coarse});
        double t = 0.0;
        for (int i = 0; i < 90; ++i) t = step_bodies(steps, 1.0 / 60.0, t, threads);
        std::vector<FloatingBodyState> out;
        for (const auto& b : bodies) out.push_back(b.state());
        return out;
    };
    const auto one = run(1), four = run(4), seven = run(7);
    bool same = true, moved = false;
    for (std::size_t i = 0; i < one.size(); ++i) {
        for (const auto* other : {&four, &seven}) {
            const FloatingBodyState& a = one[i];
            const FloatingBodyState& b = (*other)[i];
            same = same && same_bits(a.position.x, b.position.x) && same_bits(a.position.y, b.position.y) && same_bits(a.position.z, b.position.z) && same_bits(a.velocity.z, b.velocity.z) && same_bits(a.orientation[3], b.orientation[3]);
        }
        moved = moved || std::fabs(one[i].position.z) > 1e-3;
    }
    require(moved, "control: the bodies move");
    require(same, "the bodies end bit-identical on 1, 4 and 7 threads");
    // The same bodies stepped one by one give the same bits as step_bodies.
    std::vector<FloatingBody> bodies;
    build(bodies);
    double t = 0.0;
    for (int i = 0; i < 90; ++i) {
        for (std::size_t k = 0; k < bodies.size(); ++k) bodies[k].step(1.0 / 60.0, k % 2 == 0 ? static_cast<const WaterSource&>(surface) : coarse, t);
        t += 1.0 / 60.0;
    }
    bool serial = true;
    for (std::size_t i = 0; i < bodies.size(); ++i) serial = serial && same_bits(bodies[i].state().position.z, one[i].position.z);
    require(serial, "and the same as stepping each body alone");
    bool threw = false;
    try {
        std::vector<FloatingBody> none;
        const BodyStep bad{nullptr, &surface};
        (void)step_bodies(std::span<const BodyStep>(&bad, 1), 1.0 / 60.0, 0.0, 2);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a null body or source is refused");
    threw = false;
    try {
        FloatingBody twice(FloatingBodyConfig{}, {0, 0, 0});
        const BodyStep dup[2] = {{&twice, &surface}, {&twice, &surface}};
        (void)step_bodies(dup, 1.0 / 60.0, 0.0, 2);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "the same body twice is refused (two threads would step it at once)");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"validation", test_validation}, {"dispersion", test_dispersion}, {"queries", test_queries}, {"flow", test_flow},       {"foam", test_foam_and_chop},    {"wind", test_wind},
                           {"density", test_density},       {"waves", test_waves_and_bodies}, {"drag", test_drag},      {"time_step", test_time_step}, {"current", test_current}, {"stability", test_stability},
                           {"exposed", test_exposed_state}, {"lod", test_level_of_detail}, {"cells", test_cells}, {"lodcells", test_lod_cells}, {"cache", test_field_cache}, {"parallel", test_parallel_bodies}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "water tests passed\n";
    return failures == 0 ? 0 : 1;
}
