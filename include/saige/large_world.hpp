#pragma once

// Large-world positions. SAIGE keeps two kinds of coordinates and no third: a global position is a chunk cell (integer) plus a local single-precision offset inside the cell, which is the same thing as a fixed-point
// coordinate with 32-bit cell indices; every simulation and renderer works in one chunk-local single-precision frame (the physics envelope of +-256 m, capsule_sweep.hpp) and is moved between frames by origin migration
// (CapsuleCharacter::migrate_origin, Vehicle::migrate_origin, DynamicsWorld::shift_origin). Where a global quantity is needed (a distance between far apart things, the position to save, the camera relative to a
// chunk) it is computed in double from the cell difference, which is exact, never by subtracting two large floats. A float world position at 100 km resolves 7.8 mm and at 1000 km 6.3 cm; the local part of a position
// never exceeds the cell size, so its resolution is that of the cell (15 micrometers for 256 m cells) wherever in the world it is. docs/large-world.md has the measured table.
#include "saige/chunk_coord.hpp"
#include "saige/world.hpp"

namespace saige {

struct DVec3 {
    double x{}, y{}, z{};
    bool operator==(const DVec3&) const = default;
};

// A position: the cell of the x/y grid and the offset from the cell's minimum corner (x and y in [0, cell_size), z in meters, not chunked).
struct WorldPosition {
    ChunkCoord cell{};
    Vec3 local;
    bool operator==(const WorldPosition&) const = default;
};

// The cell sizes the functions accept: positive, finite and not above 4096 m (above that a float offset would resolve more than half a millimeter).
// Throws std::invalid_argument for a non-finite coordinate or bad cell size and std::out_of_range when the cell index leaves the int32 grid.
[[nodiscard]] WorldPosition to_world_position(const DVec3& global, double cell_size);
[[nodiscard]] DVec3 to_global(const WorldPosition& position, double cell_size);
// The position in the frame whose origin is the minimum corner of `frame_cell`, as floats: exact (to the float resolution of the result) for any pair of positions, however far from the global origin.
[[nodiscard]] Vec3 relative_to(const WorldPosition& position, ChunkCoord frame_cell, double cell_size);
// The vector from `from` to `to` in double: the cell difference is exact, so two points a millimeter apart across a cell border at any distance differ by a millimeter.
[[nodiscard]] DVec3 difference(const WorldPosition& from, const WorldPosition& to, double cell_size);
// Moves a position by `delta` (meters, double) and renormalizes: the local part stays in [0, cell_size) however the delta crosses cell borders.
[[nodiscard]] WorldPosition advance(const WorldPosition& position, const DVec3& delta, double cell_size);
// The shift to hand to an origin migration so that a body at `local` (in the frame of `from`) is in the frame whose origin is the minimum corner of `to`: (from - to) * cell_size, exact for power-of-two cell sizes.
[[nodiscard]] Vec3 migration_shift(ChunkCoord from, ChunkCoord to, double cell_size);
// The size of one step of a float at `value` (the position resolution there); for the tables and the tests.
[[nodiscard]] double float_resolution(double value) noexcept;

} // namespace saige
