#pragma once

// What the audio runtime needs to be told, decided by the simulation and independent of any audio library (the runtime itself is still to be chosen: docs/assets/audio.md): how an engine sounds from its
// published speed and throttle, and which impact sound a collision calls for. Pure, deterministic functions and a small state: the same inputs give the same numbers, and the audio thread can run
// any backend from them (a loop player with pitch and gain per layer, a one-shot player with a category, gain and pitch).
//
// Engine: three recorded loops (low, mid, high engine speed) are played together, each pitched by the engine speed over the speed it was recorded at (so a loop sounds natural at its own speed and is
// bent up or down around it), and crossfaded by engine speed with equal-power gains (the squares of the three gains sum to 1, so the loudness does not dip between loops). The engine speed that drives
// them is smoothed (it rises faster than it falls, the way a real engine's inertia does) so a gear change or a rev-limiter bounce does not jump the pitch. Throttle sets the load: on throttle the upper loops
// are louder and the whole engine a little louder, off throttle it is quieter and duller.
//
// Impacts: the category follows the materials that met (the harder of the two decides the character, a car body is metal), the gain follows the energy the collision removed on a logarithmic scale (a
// scrape and a crash differ by orders of magnitude), and the pitch drops as the energy rises (a heavier blow sounds lower), with a small deterministic variation from a seed so repeated impacts do
// not sound identical.

#include "saige/physics_material.hpp"

#include <array>
#include <cstdint>

namespace saige {

struct EngineAudioConfig {
    std::array<float, 3> loop_rpm{1500.0f, 3500.0f, 6000.0f};  // the engine speed each loop was recorded at, ascending
    float idle_rpm = 900.0f;
    float redline_rpm = 6800.0f;
    float rise_time = 0.05f;     // s: the time constant of the smoothed engine speed going up
    float fall_time = 0.15f;     // s: and going down
    float min_pitch = 0.5f;
    float max_pitch = 2.0f;
};

struct EngineAudioState {
    float rpm = 0.0f;            // the smoothed engine speed; 0 until the first update (which starts at the given speed)
    bool started = false;
};

struct EngineAudioFrame {
    std::array<float, 3> pitch{1, 1, 1};  // playback rate of each loop
    std::array<float, 3> gain{0, 0, 0};   // 0..1 each (equal power)
    float master = 0.0f;                   // 0..1
    float rpm = 0.0f;                      // the smoothed engine speed that produced them
};

// Throws std::invalid_argument for a configuration whose loop speeds are not ascending and positive, a non-positive time or pitch bound, or for non-finite input or a non-positive dt.
EngineAudioFrame engine_audio(const EngineAudioConfig& config, EngineAudioState& state, float rpm, float throttle, float dt);

enum class ImpactCategory : std::uint8_t { Soft, Hard, Granular, Metal, Glassy, Dull };

struct ImpactSound {
    ImpactCategory category = ImpactCategory::Hard;
    float gain = 0.0f;   // 0..1
    float pitch = 1.0f;
};

// The sound of a collision between a car body (or any object whose material is `object`) and a surface, for `energy` joules removed from the object. `object_is_metal` makes the body's own character
// metal; the surface decides the rest. Below `quiet_energy` the gain is 0 (nothing to play), at `loud_energy` and above it is 1. Throws for non-finite or negative energy.
[[nodiscard]] ImpactSound impact_sound(Surface surface, bool object_is_metal, float energy, std::uint32_t seed, float quiet_energy = 50.0f, float loud_energy = 100000.0f);

} // namespace saige
