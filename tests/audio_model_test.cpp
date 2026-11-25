// The audio model: an engine's loop pitches and equal-power crossfade from its published engine speed and throttle, the smoothing of the engine speed, and the impact sound a collision calls for.
#include "saige/audio_model.hpp"
#include "saige/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

bool throws(const EngineAudioConfig& c, float rpm, float throttle, float dt) {
    EngineAudioState s;
    try {
        (void)engine_audio(c, s, rpm, throttle, dt);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_crossfade_and_pitch() {
    const EngineAudioConfig c;
    // A first update starts at the given speed (nothing to smooth): each loop is pitched by the speed over its recorded one.
    for (float rpm = 300.0f; rpm <= 7200.0f; rpm += 50.0f) {
        EngineAudioState s;
        const EngineAudioFrame f = engine_audio(c, s, rpm, 0.0f, 0.01f);
        for (std::size_t i = 0; i < 3; ++i) {
            const float want = std::clamp(std::max(rpm, c.idle_rpm * 0.5f) / c.loop_rpm[i], c.min_pitch, c.max_pitch);
            require(std::fabs(f.pitch[i] - want) < 1e-5f, "a loop is pitched by the engine speed over its recorded speed");
        }
        require(f.master >= 0.0f && f.master <= 1.0f, "master gain in range");
    }
    // At a loop's own speed it plays at its natural pitch and carries all the gain (off throttle: the load factors are 0.75 and 0.55 for the low and high loops).
    for (std::size_t i = 0; i < 3; ++i) {
        EngineAudioState s;
        const EngineAudioFrame f = engine_audio(c, s, c.loop_rpm[i], 1.0f, 0.01f);
        require(std::fabs(f.pitch[i] - 1.0f) < 1e-5f, "a loop plays at its natural pitch at its own recorded speed");
        for (std::size_t j = 0; j < 3; ++j) {
            if (j != i) require(f.gain[j] < 1e-5f, "and the others are silent there");
        }
        require(f.gain[i] > 0.7f, "and it carries the gain");
    }
    // Equal power with the load factors divided out, over a sweep: sum of squares is 1 at every speed and throttle.
    for (float throttle : {0.0f, 0.3f, 1.0f}) {
        for (float rpm = 400.0f; rpm <= 7000.0f; rpm += 37.0f) {
            EngineAudioState s;
            const EngineAudioFrame f = engine_audio(c, s, rpm, throttle, 0.01f);
            const float g0 = f.gain[0] / (1.0f - 0.25f * throttle), g1 = f.gain[1], g2 = f.gain[2] / (0.55f + 0.45f * throttle);
            require(std::fabs(g0 * g0 + g1 * g1 + g2 * g2 - 1.0f) < 1e-4f, "the crossfade is equal power: the squares of the gains sum to 1 at every speed");
        }
    }
    // Pitch rises with engine speed, monotonically, for each loop (until the bound).
    for (std::size_t i = 0; i < 3; ++i) {
        float last = 0.0f;
        for (float rpm = 500.0f; rpm <= 6800.0f; rpm += 100.0f) {
            EngineAudioState s;
            const float p = engine_audio(c, s, rpm, 0.5f, 0.01f).pitch[i];
            require(p >= last - 1e-6f, "pitch never falls as the engine speeds up");
            last = p;
        }
    }
    // Load: more throttle is louder, and brighter (a bigger share on the high loop).
    EngineAudioState a, b;
    const EngineAudioFrame off = engine_audio(c, a, 4800.0f, 0.0f, 0.01f), on = engine_audio(c, b, 4800.0f, 1.0f, 0.01f);
    require(on.master > off.master + 0.2f && on.gain[2] > off.gain[2] * 1.5f, "on throttle the engine is louder and brighter than off it");
    std::printf("engine at 4800 rpm: off throttle master %.2f, on %.2f; high loop gain %.2f against %.2f\n", static_cast<double>(off.master), static_cast<double>(on.master), static_cast<double>(off.gain[2]), static_cast<double>(on.gain[2]));
}

void test_smoothing() {
    const EngineAudioConfig c;
    // A step up from 3000 to 6000 rpm: it rises monotonically without overshoot, reaches 63% in about the rise time, and takes longer to come back down.
    EngineAudioState s;
    (void)engine_audio(c, s, 3000.0f, 1.0f, 0.01f);
    float t63_up = -1.0f, last = 3000.0f;
    bool monotone = true;
    for (int i = 1; i <= 300; ++i) {
        const float r = engine_audio(c, s, 6000.0f, 1.0f, 0.005f).rpm;
        monotone = monotone && r >= last - 1e-3f && r <= 6000.001f;
        last = r;
        if (t63_up < 0.0f && r >= 3000.0f + 0.632f * 3000.0f) t63_up = static_cast<float>(i) * 0.005f;
    }
    require(monotone, "the smoothed engine speed rises monotonically and never overshoots");
    require(std::fabs(t63_up - c.rise_time) < 0.01f, "it covers 63% of a step in the rise time constant");
    float t63_down = -1.0f;
    for (int i = 1; i <= 600; ++i) {
        const float r = engine_audio(c, s, 3000.0f, 0.0f, 0.005f).rpm;
        if (t63_down < 0.0f && r <= 6000.0f - 0.632f * 3000.0f) t63_down = static_cast<float>(i) * 0.005f;
    }
    std::printf("engine speed smoothing: 63%% up in %.3f s, down in %.3f s\n", static_cast<double>(t63_up), static_cast<double>(t63_down));
    require(t63_down > 2.0f * t63_up, "it falls more slowly than it rises (engine inertia)");
    // A gear change: the engine speed drops 2000 rpm in a quarter second; the pitch of each loop changes by no more than a fixed slew per 60 Hz tick (no pop).
    EngineAudioState g;
    (void)engine_audio(c, g, 5800.0f, 1.0f, 0.01f);
    float prev_pitch = engine_audio(c, g, 5800.0f, 1.0f, 1.0f / 60.0f).pitch[2], worst = 0.0f;
    for (int i = 0; i < 90; ++i) {
        const float target = i < 15 ? 5800.0f - 2000.0f * static_cast<float>(i) / 15.0f : 3800.0f;
        const float p = engine_audio(c, g, target, 0.0f, 1.0f / 60.0f).pitch[2];
        worst = std::max(worst, std::fabs(p - prev_pitch));
        prev_pitch = p;
    }
    std::printf("gear change: largest high-loop pitch change per tick %.4f\n", static_cast<double>(worst));
    require(worst < 0.03f, "a gear change moves the pitch smoothly (under 3% per 60 Hz tick)");
    // Deterministic: the same sequence gives the same bits.
    EngineAudioState x, y;
    float fx = 0.0f, fy = 0.0f;
    for (int i = 0; i < 200; ++i) {
        fx = engine_audio(c, x, 1000.0f + 25.0f * static_cast<float>(i), 0.4f, 1.0f / 60.0f).master;
        fy = engine_audio(c, y, 1000.0f + 25.0f * static_cast<float>(i), 0.4f, 1.0f / 60.0f).master;
    }
    require(fx == fy, "deterministic");
    // Bad input.
    require(throws(c, std::nanf(""), 0.0f, 0.01f) && throws(c, 1000.0f, 0.0f, 0.0f) && throws(c, 1000.0f, std::nanf(""), 0.01f), "non-finite input and a zero dt are rejected");
    EngineAudioConfig bad = c;
    bad.loop_rpm = {3000.0f, 2000.0f, 6000.0f};
    require(throws(bad, 1000.0f, 0.0f, 0.01f), "loops that are not ascending are rejected");
    bad = c;
    bad.fall_time = 0.0f;
    require(throws(bad, 1000.0f, 0.0f, 0.01f), "a zero time constant is rejected");
}

void test_with_the_vehicle() {
    // The published engine speed of the powertrain drives it: a full-throttle launch through the gears keeps every loop's pitch bounded and the master gain in range, and the pitch of the leading loop rises
    // while accelerating in a gear and drops at the shifts.
    std::vector<Aabb> boxes{{{-5000, -5000, -2}, {5000, 5000, 0}}};
    const StaticCollision world(boxes, {});
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 1.0f;
    const EngineAudioConfig c;
    EngineAudioState s;
    int shifts = 0, last_gear = 1;
    float max_master = 0.0f, min_pitch = 9.0f, max_pitch = 0.0f;
    float prev_rpm_pitch = 0.0f;
    bool rises_in_gear = true;
    for (int i = 0; i < 60 * 10; ++i) {
        v.step(in, 1.0f / 60.0f, world, scratch);
        const EngineAudioFrame f = engine_audio(c, s, v.state().engine_rpm, in.throttle, 1.0f / 60.0f);
        if (v.state().gear != last_gear) {
            ++shifts;
            last_gear = v.state().gear;
        } else if (v.state().gear == last_gear && i > 60 && f.pitch[1] < prev_rpm_pitch - 0.02f && shifts == 0) {
            rises_in_gear = false;
        }
        prev_rpm_pitch = f.pitch[1];
        max_master = std::max(max_master, f.master);
        for (float p : f.pitch) min_pitch = std::min(min_pitch, p), max_pitch = std::max(max_pitch, p);
    }
    std::printf("a 10 s launch: %d shifts, master up to %.2f, pitch range %.2f to %.2f\n", shifts, static_cast<double>(max_master), static_cast<double>(min_pitch), static_cast<double>(max_pitch));
    require(shifts >= 2, "control: the launch went through at least two gear changes");
    require(max_master <= 1.0f && min_pitch >= c.min_pitch && max_pitch <= c.max_pitch, "gains and pitches stay in range");
    require(rises_in_gear, "the loop's pitch does not fall while accelerating in first gear");
}

void test_impacts() {
    // Categories follow the surface; a metal body rings on hard surfaces and not on soft ones.
    require(impact_sound(Surface::Asphalt, true, 5000.0f, 1).category == ImpactCategory::Metal, "a car on asphalt: metal");
    require(impact_sound(Surface::Concrete, false, 5000.0f, 1).category == ImpactCategory::Hard, "a dropped rock on concrete: hard");
    require(impact_sound(Surface::Snow, true, 5000.0f, 1).category == ImpactCategory::Soft, "a car in snow thuds");
    require(impact_sound(Surface::Gravel, true, 5000.0f, 1).category == ImpactCategory::Granular, "gravel is granular");
    require(impact_sound(Surface::Rubber, true, 5000.0f, 1).category == ImpactCategory::Dull, "a rubber barrier is dull");
    require(impact_sound(Surface::Ice, false, 5000.0f, 1).category == ImpactCategory::Glassy, "ice is glassy");
    // Gain: silent under the quiet threshold, rising with the logarithm of the energy, full at the loud one.
    require(impact_sound(Surface::Asphalt, true, 10.0f, 1).gain == 0.0f, "a tap under 50 J is silent");
    float last = 0.0f;
    for (float e = 60.0f; e <= 200000.0f; e *= 1.5f) {
        const float g = impact_sound(Surface::Asphalt, true, e, 1).gain;
        require(g >= last - 1e-6f && g >= 0.0f && g <= 1.0f, "gain rises with the energy and stays in range");
        last = g;
    }
    require(std::fabs(impact_sound(Surface::Asphalt, true, 100000.0f, 1).gain - 1.0f) < 1e-5f && impact_sound(Surface::Asphalt, true, 1e7f, 1).gain == 1.0f, "full gain at and above the loud energy");
    const float mid = impact_sound(Surface::Asphalt, true, std::sqrt(50.0f * 100000.0f), 1).gain;
    require(std::fabs(mid - 0.5f) < 0.01f, "the geometric mean of the thresholds is half gain (a logarithmic scale)");
    // Pitch: lower for a bigger blow on average; the seed varies it by at most 6% and deterministically.
    float small_sum = 0.0f, big_sum = 0.0f;
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        const ImpactSound a = impact_sound(Surface::Asphalt, true, 100.0f, seed), b = impact_sound(Surface::Asphalt, true, 80000.0f, seed);
        small_sum += a.pitch, big_sum += b.pitch;
        require(a.pitch == impact_sound(Surface::Asphalt, true, 100.0f, seed).pitch, "deterministic for a seed");
    }
    require(small_sum / 200.0f > big_sum / 200.0f + 0.2f, "a bigger blow sounds lower on average");
    bool varies = false;
    for (std::uint32_t seed = 2; seed < 20; ++seed) varies = varies || impact_sound(Surface::Asphalt, true, 3000.0f, seed).pitch != impact_sound(Surface::Asphalt, true, 3000.0f, 1).pitch;
    require(varies, "the seed varies the pitch");
    for (std::uint32_t seed = 0; seed < 5000; ++seed) {
        const float p = impact_sound(Surface::Asphalt, true, 3000.0f, seed).pitch;
        const float centre = impact_sound(Surface::Asphalt, true, 3000.0f, 0).pitch;
        require(std::fabs(p - centre) <= 0.13f, "the variation is small");
    }
    bool threw = false;
    try {
        (void)impact_sound(Surface::Asphalt, true, -1.0f, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "negative energy is rejected");
    // From a real collision: the vehicle's impact events carry the energy; a hard hit into a wall is loud and a bump quiet.
    std::vector<Aabb> boxes{{{-1000, -1000, -2}, {1000, 1000, 0}}, {{0, -50, 0}, {1, 50, 5}}};
    const StaticCollision world(boxes, {});
    QueryScratch scratch;
    double loud = 0.0, quiet = 0.0;
    for (const float speed : {2.0f, 12.0f}) {
        VehicleConfig cfg;
        cfg.powertrain.enabled = true;
        Vehicle v(cfg, {-30, 0, 0.9f}, 0.0f);
        VehicleInput in;
        double energy = 0.0;
        for (int i = 0; i < 60 * 40 && energy == 0.0; ++i) {
            in.throttle = std::clamp((speed - v.state().forward_speed) * 0.6f, -0.5f, 1.0f);
            v.step(in, 1.0f / 60.0f, world, scratch);
            for (const VehicleImpact& im : v.take_impacts()) energy += static_cast<double>(im.energy);
        }
        const float g = impact_sound(Surface::Concrete, true, static_cast<float>(energy), 7).gain;
        (speed > 5.0f ? loud : quiet) = static_cast<double>(g);
        std::printf("impact at %.0f m/s: %.0f J, gain %.2f\n", static_cast<double>(speed), energy, static_cast<double>(g));
    }
    require(loud > quiet + 0.2 && loud > 0.8, "a 12 m/s crash is loud and a 2 m/s bump much quieter");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"crossfade", test_crossfade_and_pitch}, {"smoothing", test_smoothing}, {"vehicle", test_with_the_vehicle}, {"impacts", test_impacts}};
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
    if (failures == 0) std::cout << "audio model tests passed\n";
    return failures == 0 ? 0 : 1;
}
