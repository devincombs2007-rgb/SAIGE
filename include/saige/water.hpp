#pragma once

// A reusable water module: a time-dependent wave surface with height, normal, flow and foam queries, and floating bodies that respond to it (issue #80). Independent of rendering and of the vehicle; a host asks
// the surface questions and feeds a body the time.
//
// The surface is a sum of Gerstner (trochoidal) waves, the standard real-time ocean: each wave has a direction, wavelength and amplitude, and its speed follows the dispersion relation of water of the configured
// depth (omega^2 = g k tanh(k d); deep water when the depth is 0), so long waves outrun short ones and waves slow in shallow water. A surface point of the sum moves in a circle in the plane of its wave, so crests
// sharpen and troughs flatten by the `choppiness`. The height at a world position is exact (the horizontal displacement is inverted by fixed-point iteration, which converges when choppiness * sum(k A) < 1,
// enforced by validation), the normal is analytic, the flow is the orbital velocity of each wave (decaying with depth) plus a uniform current, and the foam indicator is the Jacobian of the horizontal
// displacement: it falls toward 0 where the surface is about to fold over (a breaking crest). Everything is deterministic: the same configuration and time give the same numbers, with no random source; `from_wind`
// builds a wave set from a Pierson-Moskowitz spectrum with fixed phases.
//
// A `FloatingBody` is a rigid box of a given density filled with sample cells. Each cell feels buoyancy in proportion to how far it is under the surface at its own position (so a body follows the waves and a long body
// averages them), quadratic drag against the water's flow there (so a current pushes it, and a moving body slows), and gravity acts on the whole. No added mass, no wave radiation or diffraction, no
// planing or slamming; one rigid body per instance and no body-body contact. The submerged cells are exposed (`wetted()`), and the submerged volume and waterline position, so a host can later spawn wakes, foam
// or splashes from a moving body; swimming characters, coastal shoaling (the depth already enters the dispersion) and weather (wind sets the sea state) are extensions of these two types, not of their callers.
//
// Frame: +Z up, the mean surface at z = `level`. Time is in seconds as a double (it grows without bound). Positions in meters, chunk-local like the rest of the physics.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace saige {

struct WaterVec {
    double x{}, y{}, z{};
};

struct WaterWave {
    double direction = 0.0;      // rad, the direction of travel in the horizontal plane (0 = +X, counter-clockwise from above)
    double wavelength = 20.0;    // m
    double amplitude = 0.3;      // m (crest height above the mean; the wave height is twice this)
    double phase = 0.0;          // rad
};

struct WaterConfig {
    double level = 0.0;                 // m: the mean surface height
    double depth = 0.0;                 // m of water: 0 means deep (the dispersion relation of deep water and the full orbital decay)
    double density = 1000.0;            // kg/m^3 (fresh water 1000, sea water about 1025)
    double gravity = 9.81;
    double choppiness = 0.0;            // 0 (pure vertical motion) to 1 (as Gerstner as the waves allow: validated so the surface never folds)
    WaterVec current{};                 // m/s, uniform (horizontal)
    std::vector<WaterWave> waves;
};

// Throws std::invalid_argument for non-finite values, a wavelength under 0.5 m, a negative amplitude or one above a tenth of its wavelength (a wave steeper than that is not a Gerstner wave), a depth in (0, 0.5), a
// density or gravity not positive, more than 64 waves, or choppiness * sum(k A) not below 0.95.
void validate_water_config(const WaterConfig& config);

// A deterministic sea state for a wind of `speed` m/s toward `direction`: `count` waves spread over the Pierson-Moskowitz spectrum of a fully developed sea (peak at omega_p = 0.877 g / U), each with a
// direction within +-35 degrees of the wind and a fixed phase. The significant wave height of the sea it describes is about 0.21 U^2 / g.
[[nodiscard]] std::vector<WaterWave> waves_from_wind(double speed, double direction, int count = 12, double depth = 0.0, double gravity = 9.81);

// What a floating body asks of the water: the height at a place and time and the flow there. WaterSurface answers exactly; WaterFieldCache answers from values shared between bodies; both are safe to read from
// several threads at once as long as nothing is being prepared (see WaterFieldCache).
class WaterSource {
public:
    virtual ~WaterSource() = default;
    [[nodiscard]] virtual const WaterConfig& config() const noexcept = 0;
    // The height at (x, y) and the flow at min(z, height) there; the flow is only computed (else zero) when the height is above `flow_only_above`.
    virtual void sample(double x, double y, double z, double t, double& height_out, WaterVec& flow_out, double flow_only_above = -1e300) const = 0;
};

class WaterSurface : public WaterSource {
public:
    explicit WaterSurface(const WaterConfig& config);  // throws std::invalid_argument (validate_water_config)

    [[nodiscard]] const WaterConfig& config() const noexcept override { return config_; }
    // The same sea without its waves shorter than `min_wavelength` (a coarser level of detail: a large or distant body averages short waves out anyway). Every other setting is kept.
    [[nodiscard]] WaterSurface filtered(double min_wavelength) const;
    // The same sea with only its `count` largest waves by amplitude (ties by the longer wave): a coarse level of detail that keeps the energy of the sea. Throws for a count of 0 when the sea has waves.
    [[nodiscard]] WaterSurface reduced(std::size_t count) const;
    [[nodiscard]] std::size_t wave_count() const noexcept { return config_.waves.size(); }
    // The surface height at a world position and time.
    [[nodiscard]] double height(double x, double y, double t) const;
    // The unit surface normal (pointing up) there.
    [[nodiscard]] WaterVec normal(double x, double y, double t) const;
    // The water's velocity at a world point (below the surface it decays with depth; above it the surface's own value): the orbital motion of every wave plus the current.
    [[nodiscard]] WaterVec velocity(double x, double y, double z, double t) const;
    // The height at a world position and the flow at min(z, height) there, from one inversion of the displacement (what a body asks for each sample cell: about 40% cheaper than height() followed by velocity(); the flow is computed only when the height is above `flow_only_above`, else it is zero).
    void sample(double x, double y, double z, double t, double& height_out, WaterVec& flow_out, double flow_only_above = -1e300) const override;
    // The Jacobian of the horizontal displacement at a world position: 1 for a calm surface, falling toward 0 where the surface is folding (a breaking crest), never negative for a valid configuration.
    [[nodiscard]] double jacobian(double x, double y, double t) const;
    // Foam in [0, 1]: 0 where the Jacobian is above `threshold` (0.8), rising to 1 at 0.
    [[nodiscard]] double foam(double x, double y, double t, double threshold = 0.8) const;
    // The angular frequency and wavenumber of wave i (the dispersion relation of this depth), and its phase speed.
    [[nodiscard]] double wavenumber(std::size_t i) const { return k_[i]; }
    [[nodiscard]] double omega(std::size_t i) const { return w_[i]; }
    [[nodiscard]] double phase_speed(std::size_t i) const { return w_[i] / k_[i]; }

private:
    // The undisplaced coordinates whose displaced position is (x, y) at time t.
    void source(double x, double y, double t, double& x0, double& y0) const;
    double height_at_source(double x0, double y0, double t) const;
    WaterVec velocity_at_source(double x0, double y0, double z, double t) const;
    WaterConfig config_;
    std::vector<double> k_, w_, cx_, cy_;
};

// Surface values computed once and shared by every body near them. `begin(t0, t1)` starts a frame interval, `cover` computes the tiles around a body (4 x 4 points of `spacing` meters, height and the flow at the surface, at t0 and
// at t1), and `sample` interpolates bilinearly in space and linearly in time (a frame is short against a wave period, so linear in time costs about nothing). A sample outside the interval, the prepared tiles or a sea of finite
// depth falls back to the exact surface (counted in `fallbacks()`), so the result is always an answer. The flow below the surface is the surface flow decayed with one wavenumber (the amplitude-weighted mean of the sea's), the
// one approximation besides the interpolation, and it is measured, not assumed: for a twelve-wave sea (60 m down to 7 m) the horizontal flow error is 0% at the surface, 4% at 0.25 m under it, 8% at 0.5 m, 14% at 0.9 m and 39% at 3 m:
// a flow asked for deeper than `max_flow_depth` below the surface falls back to the exact answer too. `begin` and `cover` change the cache and must not run while another thread samples; `sample` only reads (and bumps an atomic counter).
class WaterFieldCache : public WaterSource {
public:
    explicit WaterFieldCache(const WaterSurface& surface, double spacing = 0.5, double max_flow_depth = 0.25);
    void begin(double t0, double t1);
    void cover(double x, double y, double radius);
    [[nodiscard]] const WaterConfig& config() const noexcept override { return surface_.config(); }
    void sample(double x, double y, double z, double t, double& height_out, WaterVec& flow_out, double flow_only_above = -1e300) const override;
    [[nodiscard]] std::size_t tile_count() const noexcept { return tiles_.size(); }
    [[nodiscard]] std::size_t points_computed() const noexcept { return points_; }
    [[nodiscard]] std::size_t fallbacks() const noexcept { return fallbacks_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    static constexpr int tile_size = 4;
    struct Tile {
        std::array<double, tile_size * tile_size> h0, h1;
        std::array<std::array<double, 3>, tile_size * tile_size> f0, f1;
    };
    [[nodiscard]] const Tile* tile_at(std::int64_t px, std::int64_t py) const;
    void corner(std::int64_t px, std::int64_t py, double& h0, double& h1, std::array<double, 3>& f0, std::array<double, 3>& f1, bool& ok) const;
    const WaterSurface& surface_;
    double spacing_;
    double t0_ = 0.0, t1_ = 0.0;
    bool open_ = false, usable_ = false;
    double k_dom_ = 0.0, max_flow_depth_ = 0.25;
    std::unordered_map<std::int64_t, Tile> tiles_;
    std::size_t points_ = 0;
    mutable std::atomic<std::size_t> fallbacks_{0};
};

struct FloatingBodyConfig {
    WaterVec half_extents{1.0, 1.0, 0.5};   // the box, m
    double density = 500.0;                  // kg/m^3 of the whole body (its mass is this times the volume, unless `mass` is set)
    double mass = 0.0;                       // kg; 0 means density * volume
    WaterVec center_of_mass{};               // relative to the box center (a negative z is ballast: more stable)
    int cells = 6;                           // sample cells per axis (at least 2, at most 16)
    double drag_coefficient = 1.0;           // quadratic drag against the water's flow, on the mean projected area
    double angular_drag = 1.0;               // multiplier of the rotational drag from the cells' offsets (1 = as computed)
    double drag_speed_floor = 0.25;          // m/s: the drag uses |v| + this, so a slow body still meets resistance (the viscous and wave-making drag of a real hull is not purely quadratic)
    double heave_damping = 0.15;             // the damping ratio (of critical) of the heave of a surface-piercing body: the energy it radiates in waves; 0 turns it off
    double roll_damping = 0.08;              // the same for roll and pitch (of the waterplane's stiffness rho g I); 0 turns it off
};

struct FloatingBodyState {
    WaterVec position;                       // the box center
    std::array<double, 4> orientation{0, 0, 0, 1};  // x, y, z, w
    WaterVec velocity;                       // of the center of mass
    WaterVec angular_velocity;
    double submerged_volume = 0.0;           // m^3 under the surface, last step
    double waterline = 0.0;                  // m: mean surface height under the cells (or the level when none are wet), last step
};

struct WetSample {
    WaterVec position;                       // the cell center
    WaterVec velocity;                       // of the cell
    double submersion = 0.0;                 // 0 to 1
};

class FloatingBody {
public:
    static constexpr double substep = 1.0 / 240.0;
    // Places the body with its box center at `position`, level, at rest. Throws std::invalid_argument for non-finite or non-positive extents, density or mass, a center of mass outside the box, or cells out of range.
    FloatingBody(const FloatingBodyConfig& config, WaterVec position);

    // Advances from time `t` by `dt` seconds (any positive dt up to one second; it is cut into substeps of at most 1/240 s). Returns the new time. Throws std::invalid_argument for a non-finite or non-positive dt.
    double step(double dt, const WaterSource& water, double t);
    // Re-samples the body with `cells` cells per axis (2 to 16), keeping its state: a nearer body can have more, a far one fewer. The wet-cell list is cleared until the next step.
    void set_cells(int cells);

    [[nodiscard]] const FloatingBodyState& state() const noexcept { return state_; }
    [[nodiscard]] const FloatingBodyConfig& config() const noexcept { return config_; }
    [[nodiscard]] double mass() const noexcept { return mass_; }
    [[nodiscard]] double volume() const noexcept { return volume_; }
    // The principal moments of inertia about the center of mass (kg m^2): the box's, plus the parallel-axis term for a center of mass off the box center.
    [[nodiscard]] WaterVec inertia() const noexcept { return {inertia_[0], inertia_[1], inertia_[2]}; }
    // The sample cells that were under the surface in the last step (position, velocity, submersion).
    [[nodiscard]] const std::vector<WetSample>& wetted() const noexcept { return wet_; }
    // Applies an impulse (N s) at the center of mass, or sets the velocity outright.
    void set_velocity(WaterVec v) noexcept;
    // Tilts the body about its horizontal x axis (roll) by `angle` radians from level, keeping the center in place; for tests and spawns.
    void set_roll(double angle) noexcept;
    [[nodiscard]] double roll() const noexcept;   // rad about the box's x axis
    [[nodiscard]] double pitch() const noexcept;  // rad about the box's y axis

private:
    void substep_once(double dt, const WaterSource& water, double t);
    void build_cells();
    FloatingBodyConfig config_;
    FloatingBodyState state_;
    double mass_ = 0.0, volume_ = 0.0;
    double inertia_[3]{};
    struct Cell {
        double offset[3];  // from the box center, body frame
    };
    std::vector<Cell> cells_;
    double cell_volume_ = 0.0, cell_height_ = 0.0, cell_area_ = 0.0;
    double com_[3]{};
    double p_[3]{}, q_[4]{0, 0, 0, 1}, v_[3]{}, w_[3]{};
    std::vector<WetSample> wet_;
};

// The sample cells per axis a body needs at `distance` meters from the viewer, for a body that has `full_cells` up close: all of them within 40 m, two thirds (at least 3) out to 100 m, half (at least 2) beyond.
// Measured on a wind sea of Hs 2 m (tools/benchmarks/water_workloads.cpp, docs/water.md): the far bodies' height error is under a millimeter RMS and their roll error under 0.2 degrees, for a fifth of the cost. Bodies within
// 40 m keep every cell, so nearby water is exactly the reference.
[[nodiscard]] int water_lod_cells(double distance, int full_cells);

// A body and the water it should see (a body far away can be given a filtered sea or a cache of its own).
struct BodyStep {
    FloatingBody* body = nullptr;
    const WaterSource* water = nullptr;
};
// Advances every body by dt from time t, on `threads` threads (1 runs on the caller's). Bodies are independent and the sources are only read, so the result is bit-identical for any thread count. Returns t + dt.
double step_bodies(std::span<const BodyStep> bodies, double dt, double t, unsigned threads);

} // namespace saige
