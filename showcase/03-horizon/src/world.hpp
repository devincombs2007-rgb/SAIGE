// Showcase 03 "Beyond the Horizon": the world. A procedural heightfield with a road network, a small roadside town and a winding countryside road, split into
// 64 m cells that are prepared on demand (CPU: meshes, collision, prop placements) and streamed. Everything is a pure function of the cell coordinates, so a cell
// prepared on any thread, in any order, is bit-identical: the simulation depends on which cells are resident (a function of position), never on timing.
//
// Frame: world meters, +X along the main street, +Y to its left, +Z up. A cell (cx, cy) covers [cx*64, cx*64 + 64) x [cy*64, cy*64 + 64); its vertices and
// collision are stored relative to that corner so they stay precise at any distance from the origin.
#pragma once

#include "saige/static_collision.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace saige::horizon {

inline constexpr double cell_size = 64.0;

struct D2 {
    double x{}, y{};
};

enum class Surface : std::uint8_t {
    Ground, Road, Shoulder, Sidewalk, Marking, WallBrick, WallPlaster, RoofTiles, RoofMetal, Apron, WindowLit, WindowDark, Door, Bark, Foliage, LampGlow, Count
};
inline constexpr std::size_t surface_count = static_cast<std::size_t>(Surface::Count);

struct Vertex {
    float px, py, pz, nx, ny, nz, u, v;
};

struct MeshPart {
    Surface surface{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

enum class PropType : std::uint8_t { Barrier, Lamp, UtilityBox, RockLarge, RockSmall, ShrubA, ShrubB, Barrel, BarrelStack, Bench, TreePine, TreeRound, Count };

struct PropInstance {
    PropType type{};
    double x{}, y{}, z{};  // world meters
    float yaw{};           // radians about +Z
    float scale{1.0f};
};

struct CellData {
    int cx{}, cy{};
    std::vector<MeshPart> parts;
    std::vector<Triangle> triangles;  // collision, relative to the cell corner
    std::vector<Aabb> boxes;          // collision, relative to the cell corner
    std::vector<PropInstance> props;  // world coordinates
    [[nodiscard]] std::uint64_t vertex_count() const;
    [[nodiscard]] std::uint64_t triangle_count() const;
};

struct RoadSample {
    double distance{1e9};  // to the nearest centerline
    double s{};            // arc length along it
    double z{};            // the road's height there
    double side{};         // +1 left of travel, -1 right
    int road{-1};
};

struct Road {
    std::vector<D2> points;       // centerline, ~1 m apart
    std::vector<double> s;        // arc length of each point
    std::vector<double> z;        // elevation of each point
    double width = 7.2;
    bool town = false;            // sidewalks and flat terrain
};

class World {
public:
    World();

    [[nodiscard]] double height(double x, double y) const;
    [[nodiscard]] RoadSample nearest_road(double x, double y, int only_road = -1) const;
    [[nodiscard]] const std::vector<Road>& roads() const noexcept { return roads_; }
    [[nodiscard]] const Road& route() const noexcept { return roads_.front(); }
    [[nodiscard]] double route_length() const noexcept { return roads_.front().s.back(); }
    // Position and heading (radians) on the main route at arc length `s` (clamped).
    [[nodiscard]] D2 route_point(double s, double* heading = nullptr, double* z = nullptr) const;

    // Pure and thread-safe: the same cell every time.
    [[nodiscard]] CellData build_cell(int cx, int cy) const;
    // The coarse terrain of the whole region, drawn once beyond the streamed cells: a grid at `step` meters over [x0, x1] x [y0, y1], lowered by `drop`.
    [[nodiscard]] MeshPart far_terrain(double x0, double y0, double x1, double y1, double step, double drop) const;

    // The crash site: where the barriers stand (world meters) and the bend's direction of travel at the point of the impact.
    struct Landmarks {
        D2 start;
        D2 gas_station;
        D2 crash_barrier;
        double crash_heading{};
        double crash_s{};
    };
    [[nodiscard]] const Landmarks& landmarks() const noexcept { return landmarks_; }

private:
    void add_road(std::vector<D2> controls, double width, bool town, double amplitude_start);
    [[nodiscard]] double noise_height(double x, double y) const;
    [[nodiscard]] double town_weight(double x, double y) const;

    std::vector<Road> roads_;
    Landmarks landmarks_;
};

[[nodiscard]] std::uint64_t hash64(std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0);
// Uniform in [0, 1) from a hash.
[[nodiscard]] double unit(std::uint64_t h);

} // namespace saige::horizon
