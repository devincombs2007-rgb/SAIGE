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

#include "saige/audio.hpp"
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

enum class ImpactCategory : std::uint8_t { Soft, Hard, Granular, Metal, Glassy, Dull, Wood };

struct ImpactSound {
    ImpactCategory category = ImpactCategory::Hard;
    float gain = 0.0f;   // 0..1
    float pitch = 1.0f;
};

// The sound of a collision between a car body (or any object whose material is `object`) and a surface, for `energy` joules removed from the object. `object_is_metal` makes the body's own character
// metal; the surface decides the rest. Below `quiet_energy` the gain is 0 (nothing to play), at `loud_energy` and above it is 1. Throws for non-finite or negative energy.
[[nodiscard]] ImpactSound impact_sound(Surface surface, bool object_is_metal, float energy, std::uint32_t seed, float quiet_energy = 50.0f, float loud_energy = 100000.0f);

struct BreakSound {
    ImpactCategory category = ImpactCategory::Wood;
    float gain = 0.0f;    // 0..1
    float pitch = 1.0f;
    float crack = 0.0f;   // 0..1 the strength of the first transient (snap of a plank, the burst of a pane) over the tail of falling pieces
};

// The sound of a link breaking (include/saige/destruction.hpp): `severity` is the load over the limit (>= 1) and `piece_mass_kg` the lighter piece held. Glass is bright and sharp (high pitch, strong crack), wood
// dull and lower, stone and metal heavy; harder breaks are louder on a logarithmic scale and heavier pieces lower. Deterministic; throws for non-finite input, a severity below 1 or a non-positive mass.
[[nodiscard]] BreakSound break_sound(BreakMaterial material, float severity, float piece_mass_kg, std::uint32_t seed);

// ---- Footsteps, tires, water and ambience: the same kind of pure decision, from published simulation state ----

struct FootstepSound {
    ImpactCategory category = ImpactCategory::Hard;
    float gain = 0.0f;   // 0..1
    float pitch = 1.0f;
    float scuff = 0.0f;  // 0..1 how much of a slide there is in the step (a skid on a turn, a drag on a slope)
};

// A footfall at `speed` m/s horizontal travel by a body of `mass_kg`; `landing_speed` > 0 is the vertical speed of a landing (a jump or a drop adds a thud). Gain rises with speed and weight on a
// logarithmic scale from a quiet walk (about 1.4 m/s) to a hard run (about 6 m/s) and a heavy landing; the ground picks the character (grass and snow are soft, gravel and sand granular, concrete hard) and a wet hard
// surface is a little louder and higher. Throws for non-finite or negative inputs and a non-positive mass.
[[nodiscard]] FootstepSound footstep_sound(Surface surface, float speed, float mass_kg, float landing_speed, float slide_speed, std::uint32_t seed);

struct TireSound {
    ImpactCategory category = ImpactCategory::Hard;  // what the road is made of: the rolling noise and the skid take their texture from it
    float roll_gain = 0.0f;    // 0..1 rolling noise loop
    float roll_pitch = 1.0f;
    float skid_gain = 0.0f;    // 0..1 squeal on a hard surface, crunch/spray on a loose one
    float skid_pitch = 1.0f;
};

// The sound of one wheel at `speed` m/s of ground speed with `slip` m/s of sliding between the tire and the ground (the magnitude of the slip velocity, longitudinal and lateral together). Rolling noise rises with speed and
// with how coarse the surface is; skid begins above 1.5 m/s of slip and is full at 8 m/s, squeals higher on asphalt than on loose ground and louder on wet asphalt. Throws for non-finite or negative inputs.
[[nodiscard]] TireSound tire_sound(Surface surface, float speed, float slip);

struct WaterSound {
    float splash_gain = 0.0f;  // 0..1 one-shot at the entry
    float splash_pitch = 1.0f;
    float flow_gain = 0.0f;    // 0..1 lapping/wake loop while moving through water
    float flow_pitch = 1.0f;
};

// A body of `mass_kg` meeting the water at `entry_speed` m/s (0 when not entering) and moving at `speed` m/s relative to the water while `submerged_fraction` (0..1) of it is under the surface. A bigger or faster
// entry is louder and lower; flow is louder with speed and with more of the body in the water. Throws for non-finite or negative inputs, a non-positive mass or a fraction outside 0..1.
[[nodiscard]] WaterSound water_sound(float entry_speed, float mass_kg, float speed, float submerged_fraction);

struct AmbienceMix {
    float wind = 0.0f;      // 0..1
    float birds = 0.0f;     // daytime
    float insects = 0.0f;   // night (crickets) and dusk
    float rain = 0.0f;
    float traffic = 0.0f;   // distant traffic bed near roads
    float interior = 0.0f;  // 0 outside .. 1 fully enclosed: the host lowers everything and shortens the reverb
};

// The environmental bed from the time of day (hours, 0..24), wind speed (m/s), rain (0..1), how built-up the surroundings are (0..1) and how enclosed the listener is (0..1). Birds peak after dawn and before dusk, insects
// at night, rain silences the birds and insects, traffic follows built-up land and fades at night, and enclosure takes the whole bed down. Throws for non-finite input or ranges outside the documented ones.
[[nodiscard]] AmbienceMix ambience_mix(float hour, float wind_speed, float rain, float built_up, float enclosure);

// The reverb for a space: `enclosure` 0 (open air) to 1 (closed room), `volume_m3` the room's size, `hardness` 0..1 (carpet to tile). Open air has no reverb; larger and harder rooms ring longer.
[[nodiscard]] ReverbSettings reverb_for_space(float enclosure, float volume_m3, float hardness);

} // namespace saige
