#pragma once

// Buoyancy and water drag for a rigid body of the dynamics world (water meets dynamics). A box body is sampled by a grid of cells; every cell under the surface pushes the body up with the weight of the water it
// displaces and resists its motion through the water's flow, exactly as `FloatingBody` does for its own integrator (same cells, same drag law and stability limit, same heave and roll damping), but applied to a
// `DynamicsWorld` body as impulses at the cells, once per step before `world.step(dt)`. Gravity and contacts stay the dynamics world's; the world's gravity must be the water's (`WaterConfig::gravity`).
#include "saige/dynamics.hpp"
#include "saige/water.hpp"

namespace saige {

struct BuoyantBox {
    DVec half_extents{0.5, 0.5, 0.5};   // the box body's half sizes (its centre of mass is taken at the box centre)
    double mass = 0.0;                   // kg: the body's mass, as given to the world
    int cells = 6;                       // sample cells per axis (2 to 16)
    double drag_coefficient = 1.0;       // as FloatingBodyConfig
    double angular_drag = 1.0;
    double drag_speed_floor = 0.25;
    double heave_damping = 0.15;         // of critical, for a body piercing the surface; 0 off
    double roll_damping = 0.08;
};

struct BuoyancyResult {
    double submerged_volume = 0.0;       // m^3
    double waterline = 0.0;              // mean surface height under the wet cells (the water's level when none is wet)
    std::size_t wet_cells = 0;
};

// Applies the water's forces on `body` for a step of `dt` starting at time `t`. Throws std::invalid_argument for a bad box, a non-positive mass or dt, and whatever the world throws for a stale id.
BuoyancyResult apply_buoyancy(DynamicsWorld& world, BodyId body, const BuoyantBox& box, const WaterSource& water, double t, double dt);

} // namespace saige
