#include "saige/audio_model.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {

namespace {
bool fin(float x) { return std::isfinite(x); }
}

EngineAudioFrame engine_audio(const EngineAudioConfig& c, EngineAudioState& state, float rpm, float throttle, float dt) {
    if (!(c.loop_rpm[0] > 0.0f) || !(c.loop_rpm[1] > c.loop_rpm[0]) || !(c.loop_rpm[2] > c.loop_rpm[1]) || !fin(c.loop_rpm[2]) || !(c.rise_time > 0.0f) || !(c.fall_time > 0.0f) || !fin(c.rise_time) ||
        !fin(c.fall_time) || !(c.min_pitch > 0.0f) || !(c.max_pitch > c.min_pitch) || !fin(c.max_pitch) || !(c.redline_rpm > c.idle_rpm) || !(c.idle_rpm > 0.0f)) {
        throw std::invalid_argument("engine_audio: invalid configuration");
    }
    if (!fin(rpm) || !fin(throttle) || !fin(dt) || !(dt > 0.0f)) throw std::invalid_argument("engine_audio: rpm, throttle and dt must be finite and dt positive");
    rpm = std::clamp(rpm, 0.0f, c.redline_rpm * 1.1f);
    throttle = std::clamp(throttle, 0.0f, 1.0f);
    if (!state.started) {
        state.rpm = rpm;
        state.started = true;
    } else {
        const float tau = rpm > state.rpm ? c.rise_time : c.fall_time;
        state.rpm += (rpm - state.rpm) * (1.0f - std::exp(-dt / tau));
    }
    EngineAudioFrame f;
    f.rpm = state.rpm;
    const float r = std::max(state.rpm, c.idle_rpm * 0.5f);
    for (std::size_t i = 0; i < 3; ++i) f.pitch[i] = std::clamp(r / c.loop_rpm[i], c.min_pitch, c.max_pitch);
    // Crossfade weights: a triangle around each loop's speed (the first holds below its own, the last above), made equal-power by taking the square root of a linear partition.
    std::array<float, 3> w{0, 0, 0};
    if (r <= c.loop_rpm[0]) {
        w[0] = 1.0f;
    } else if (r >= c.loop_rpm[2]) {
        w[2] = 1.0f;
    } else if (r < c.loop_rpm[1]) {
        const float t = (r - c.loop_rpm[0]) / (c.loop_rpm[1] - c.loop_rpm[0]);
        w[0] = 1.0f - t;
        w[1] = t;
    } else {
        const float t = (r - c.loop_rpm[1]) / (c.loop_rpm[2] - c.loop_rpm[1]);
        w[1] = 1.0f - t;
        w[2] = t;
    }
    for (std::size_t i = 0; i < 3; ++i) f.gain[i] = std::sqrt(w[i]);
    // Load: on throttle the upper loops carry more and the whole engine is louder; off throttle it is duller and quieter.
    const float load = throttle;
    f.gain[0] *= 1.0f - 0.25f * load;
    f.gain[2] *= 0.55f + 0.45f * load;
    const float rev = std::clamp((state.rpm - c.idle_rpm) / (c.redline_rpm - c.idle_rpm), 0.0f, 1.0f);
    f.master = std::clamp(0.35f + 0.25f * rev + 0.4f * load, 0.0f, 1.0f);
    return f;
}

ImpactSound impact_sound(Surface surface, bool object_is_metal, float energy, std::uint32_t seed, float quiet_energy, float loud_energy) {
    if (!fin(energy) || energy < 0.0f || !fin(quiet_energy) || !fin(loud_energy) || !(quiet_energy > 0.0f) || !(loud_energy > quiet_energy)) throw std::invalid_argument("impact_sound: energy must be finite and not negative, with 0 < quiet < loud");
    ImpactSound s;
    switch (surface) {
    case Surface::Grass:
    case Surface::Snow: s.category = ImpactCategory::Soft; break;
    case Surface::Gravel:
    case Surface::Dirt:
    case Surface::Sand: s.category = ImpactCategory::Granular; break;
    case Surface::Ice: s.category = ImpactCategory::Glassy; break;
    case Surface::Rubber: s.category = ImpactCategory::Dull; break;
    default: s.category = ImpactCategory::Hard; break;
    }
    // A metal object on a hard surface rings; on a soft one the ground's character wins (a car on snow thuds).
    if (object_is_metal && (s.category == ImpactCategory::Hard || s.category == ImpactCategory::Glassy)) s.category = ImpactCategory::Metal;
    const float t = std::log(std::max(energy, 1e-6f) / quiet_energy) / std::log(loud_energy / quiet_energy);
    s.gain = std::clamp(t, 0.0f, 1.0f);  // the logarithm is negative below the quiet energy, so a tap is silent
    // A deterministic variation from the seed (an integer hash into [-1, 1]), and a lower pitch for a bigger blow.
    std::uint32_t h = seed * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const float variation = (static_cast<float>(h & 0xffffu) / 32767.5f - 1.0f) * 0.06f;
    s.pitch = std::clamp(1.15f - 0.35f * std::clamp(t, 0.0f, 1.0f) + variation, 0.5f, 1.5f);
    return s;
}

} // namespace saige
