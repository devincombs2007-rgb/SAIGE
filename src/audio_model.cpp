#include "saige/audio_model.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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

namespace {
float hash01(std::uint32_t seed) {
    std::uint32_t h = seed * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xffffu) / 65535.0f;
}
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
ImpactCategory category_of(Surface s) { return impact_sound(s, false, 1.0f, 0).category; }
void need_nonneg(float x, const char* what) {
    if (!std::isfinite(x) || x < 0.0f) throw std::invalid_argument(std::string(what) + " must be finite and not negative");
}
} // namespace

FootstepSound footstep_sound(Surface surface, float speed, float mass_kg, float landing_speed, float slide_speed, std::uint32_t seed) {
    need_nonneg(speed, "footstep_sound: speed");
    need_nonneg(landing_speed, "footstep_sound: landing_speed");
    need_nonneg(slide_speed, "footstep_sound: slide_speed");
    if (!std::isfinite(mass_kg) || !(mass_kg > 0.0f)) throw std::invalid_argument("footstep_sound: mass must be finite and positive");
    FootstepSound f;
    f.category = category_of(surface);
    // Energy-like measure: a step's strength rises with speed squared and weight; a landing adds its own.
    const float e = mass_kg * (0.5f * speed * speed + 0.15f + 0.5f * landing_speed * landing_speed * 4.0f);
    const float t = std::log(std::max(e, 1e-6f) / 40.0f) / std::log(8000.0f / 40.0f);
    f.gain = std::clamp(0.25f + 0.75f * std::clamp(t, 0.0f, 1.0f), 0.0f, 1.0f);
    if (speed < 0.1f && landing_speed < 0.5f) f.gain = 0.0f;  // standing still makes no footstep
    f.pitch = std::clamp(1.1f - 0.25f * std::clamp(t, 0.0f, 1.0f) + (hash01(seed) - 0.5f) * 0.1f, 0.5f, 1.5f);
    if (surface == Surface::AsphaltWet) {
        f.gain = std::min(1.0f, f.gain * 1.15f);
        f.pitch = std::min(1.5f, f.pitch * 1.04f);
    }
    f.scuff = smoothstep(0.5f, 4.0f, slide_speed);
    return f;
}

TireSound tire_sound(Surface surface, float speed, float slip) {
    need_nonneg(speed, "tire_sound: speed");
    need_nonneg(slip, "tire_sound: slip");
    TireSound t;
    t.category = category_of(surface);
    const bool loose = t.category == ImpactCategory::Granular || t.category == ImpactCategory::Soft;
    const float coarse = loose ? 1.0f : surface == Surface::Asphalt || surface == Surface::AsphaltWet ? 0.6f : 0.8f;
    t.roll_gain = std::clamp(smoothstep(0.5f, 35.0f, speed) * coarse, 0.0f, 1.0f);
    t.roll_pitch = std::clamp(0.7f + 0.025f * speed, 0.5f, 2.0f);
    const float s = smoothstep(1.5f, 8.0f, slip);
    const float wet = surface == Surface::AsphaltWet ? 1.0f : 0.8f;
    t.skid_gain = std::clamp(s * (loose ? 0.85f : wet), 0.0f, 1.0f);
    t.skid_pitch = std::clamp((loose ? 0.8f : 1.0f) + 0.02f * std::min(speed, 30.0f) - 0.1f * s, 0.5f, 2.0f);
    return t;
}

WaterSound water_sound(float entry_speed, float mass_kg, float speed, float submerged_fraction) {
    need_nonneg(entry_speed, "water_sound: entry_speed");
    need_nonneg(speed, "water_sound: speed");
    if (!std::isfinite(mass_kg) || !(mass_kg > 0.0f)) throw std::invalid_argument("water_sound: mass must be finite and positive");
    if (!std::isfinite(submerged_fraction) || submerged_fraction < 0.0f || submerged_fraction > 1.0f) throw std::invalid_argument("water_sound: submerged_fraction must be within 0..1");
    WaterSound w;
    const float e = 0.5f * mass_kg * entry_speed * entry_speed;
    if (entry_speed > 0.3f) {
        const float t = std::log(std::max(e, 1e-6f) / 5.0f) / std::log(200000.0f / 5.0f);
        w.splash_gain = std::clamp(t, 0.0f, 1.0f);
        w.splash_pitch = std::clamp(1.3f - 0.6f * std::clamp(t, 0.0f, 1.0f), 0.5f, 1.5f);
    }
    w.flow_gain = std::clamp(smoothstep(0.2f, 12.0f, speed) * (0.2f + 0.8f * submerged_fraction), 0.0f, 1.0f) * (submerged_fraction > 0.0f ? 1.0f : 0.0f);
    w.flow_pitch = std::clamp(0.8f + 0.04f * speed, 0.5f, 2.0f);
    return w;
}

AmbienceMix ambience_mix(float hour, float wind_speed, float rain, float built_up, float enclosure) {
    if (!std::isfinite(hour) || hour < 0.0f || hour > 24.0f) throw std::invalid_argument("ambience_mix: hour must be within 0..24");
    need_nonneg(wind_speed, "ambience_mix: wind_speed");
    for (const float v : {rain, built_up, enclosure}) {
        if (!std::isfinite(v) || v < 0.0f || v > 1.0f) throw std::invalid_argument("ambience_mix: rain, built_up and enclosure must be within 0..1");
    }
    AmbienceMix m;
    const float day = smoothstep(5.0f, 7.5f, hour) * (1.0f - smoothstep(18.0f, 20.5f, hour));  // 0 at night, 1 by day
    const float dusk = smoothstep(17.0f, 19.5f, hour) * (1.0f - smoothstep(21.5f, 23.5f, hour)) + smoothstep(3.5f, 5.0f, hour) * (1.0f - smoothstep(5.0f, 6.5f, hour));
    const float dry = 1.0f - rain;
    const float open = 1.0f - enclosure;
    m.wind = std::clamp(wind_speed / 15.0f, 0.0f, 1.0f) * open;
    m.birds = day * dry * (1.0f - 0.7f * built_up) * (1.0f - 0.6f * std::clamp(wind_speed / 15.0f, 0.0f, 1.0f)) * open;
    m.insects = std::clamp((1.0f - day) * 0.8f + dusk * 0.4f, 0.0f, 1.0f) * dry * (1.0f - built_up) * open;
    m.rain = rain * (1.0f - 0.6f * enclosure);
    m.traffic = built_up * (0.35f + 0.65f * day) * (1.0f - 0.8f * enclosure);
    m.interior = enclosure;
    return m;
}

ReverbSettings reverb_for_space(float enclosure, float volume_m3, float hardness) {
    if (!std::isfinite(enclosure) || enclosure < 0.0f || enclosure > 1.0f || !std::isfinite(hardness) || hardness < 0.0f || hardness > 1.0f) throw std::invalid_argument("reverb_for_space: enclosure and hardness must be within 0..1");
    if (!std::isfinite(volume_m3) || !(volume_m3 > 0.0f)) throw std::invalid_argument("reverb_for_space: volume must be finite and positive");
    ReverbSettings r;
    // Room size follows the cube root of the volume (a 30 m^3 room is small, a 30,000 m^3 hall is vast); damping falls as the surfaces harden; the return rises with enclosure.
    r.room_size = std::clamp(std::log10(volume_m3) / 5.0f, 0.0f, 1.0f);
    r.damping = std::clamp(1.0f - hardness, 0.0f, 1.0f) * 0.8f + 0.1f;
    r.wet = std::clamp(enclosure * (0.15f + 0.45f * hardness), 0.0f, 1.0f);
    return r;
}

} // namespace saige
