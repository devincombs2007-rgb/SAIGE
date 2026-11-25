// Showcase 04 "A World Alive": the district. A compact coastal urban district laid out as data (roads and lanes, intersections with signals, crosswalks, sidewalks, building lots with their kinds and
// doors, accessible interiors, a waterfront with a promenade and piers, street furniture, parking) from a seed. Everything is a pure function of the seed, so the same seed builds the same district
// on any machine. The district knows nothing about rendering: the renderer app turns this data into meshes, and the living world (world.hpp) turns it into simulation.
//
// Frame: world meters, +X east, +Y north (the sea is to the north), +Z up; the district is centred on the origin so every float stays small.
#pragma once

#include "saige/multi_floor_nav.hpp"
#include "saige/npc_simulation.hpp"
#include "saige/static_collision.hpp"
#include "saige/traffic_simulation.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace saige::alive {

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] double cx() const { return 0.5 * (x0 + x1); }
    [[nodiscard]] double cy() const { return 0.5 * (y0 + y1); }
    [[nodiscard]] double w() const { return x1 - x0; }
    [[nodiscard]] double h() const { return y1 - y0; }
    [[nodiscard]] bool contains(double x, double y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    [[nodiscard]] bool overlaps(const Rect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
};

enum class Leg : std::uint8_t { South = 0, East = 1, North = 2, West = 3 };
[[nodiscard]] inline std::array<double, 2> leg_dir(Leg l) {
    constexpr std::array<std::array<double, 2>, 4> d{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    return d[static_cast<std::size_t>(l)];
}

enum class BuildingKind : std::uint8_t { Cafe, Shop, Bakery, Bar, Pharmacy, Apartments, Offices, Hotel, Warehouse, FireStation, Count };
[[nodiscard]] const char* kind_name(BuildingKind k);
[[nodiscard]] bool is_commercial(BuildingKind k);

struct Building {
    std::uint32_t id = 0;
    std::string name;
    Rect footprint;
    int floors = 3;
    double floor_height = 3.2;
    BuildingKind kind = BuildingKind::Apartments;
    Leg front = Leg::South;        // the side with the street door
    double door_x = 0, door_y = 0; // the door's centre on the facade line
    int palette = 0;               // facade colour family
    bool interior = false;         // accessible: the ground floor (and for the cafe a second floor) is modelled and walkable
    world_sim::VenueId venue = world_sim::kInvalidVenueId;
    [[nodiscard]] double height() const { return floors * floor_height; }
};

struct StreetProp {
    std::string type;  // lamp, signal_pole, bench, bin, hydrant, tree, bollard, planter, bus_stop, bike_rack, sign_post, cafe_table, cafe_chair, crate, awning_post ...
    double x = 0, y = 0, z = 0, yaw = 0, scale = 1.0;
};

struct Intersection {
    std::uint32_t id = 0;
    double x = 0, y = 0;
    std::array<bool, 4> has_leg{};
    std::array<traffic::LaneId, 4> inbound{};   // approach lane per leg (0 where the leg does not exist)
    std::array<traffic::LaneId, 4> outbound{};
    std::array<std::uint32_t, 4> crosswalk{};   // crosswalk id per leg (0 none)
};

struct Crosswalk {
    std::uint32_t id = 0;
    std::uint32_t intersection = 0;
    Leg leg = Leg::South;
    Rect strip;                                  // the painted strip
    std::array<double, 2> a{}, b{};              // the two curb ends (pedestrian side nodes)
    std::vector<std::uint32_t> traffic_zones;    // zone ids in the traffic simulation
};

struct ParkingSpot {
    double x = 0, y = 0, yaw = 0;
    bool taken = false;
};

struct PedNode {
    double x = 0, y = 0;
    std::uint8_t layer = 0;  // 0 outdoors/ground floor, 1 first floor of an interior
    std::uint32_t crosswalk = 0;
};
struct PedEdge {
    std::uint32_t a = 0, b = 0;
    std::uint32_t crosswalk = 0;  // nonzero: a crossing, only walked on the walk signal
};

struct Pier {
    Rect deck;
    double z = 0.25;
};

struct BoatSpawn {
    double x = 0, y = 0, yaw = 0;
    double length = 4.0, beam = 1.6;
    bool underway = false;
};

struct Interior {
    std::uint32_t building = 0;
    Rect room;                                    // the ground floor's floor area (inside the walls)
    std::vector<Rect> walls;                      // wall pieces for collision and rendering, floor-to-ceiling
    std::vector<StreetProp> furniture;            // tables, chairs, counters, shelves ...
    double ceiling = 3.0;
    bool has_stairs = false;
    Rect stair_footprint;
    std::array<double, 2> stair_bottom{}, stair_top{};
    double upper_floor_z = 0.0;
    Rect upper_room;
    std::vector<Rect> upper_walls;
    std::vector<StreetProp> upper_furniture;
    std::array<double, 2> door_inside{};          // just inside the street door
    std::array<double, 2> door_outside{};         // just outside, on the sidewalk
};

struct District {
    std::uint32_t seed = 0;
    Rect bounds;
    std::vector<Rect> roadway;       // asphalt rectangles
    std::vector<Rect> sidewalk;      // paved rectangles
    std::vector<Rect> park;
    Rect promenade;
    double quay_y = 0;
    double sea_level = -1.1;
    std::vector<Building> buildings;
    std::vector<Intersection> intersections;
    std::vector<Crosswalk> crosswalks;
    std::vector<StreetProp> props;
    std::vector<ParkingSpot> parking;
    std::vector<Pier> piers;
    std::vector<BoatSpawn> boats;
    std::vector<Interior> interiors;
    std::vector<PedNode> ped_nodes;
    std::vector<PedEdge> ped_edges;
    std::vector<world_sim::Venue> venues;
    std::vector<Aabb> collision;     // static collision for characters, vehicles and bodies: buildings, curbs, props, piers, bulkhead
    traffic::TrafficSimulation traffic;  // lanes, intersections, crosswalk zones (no vehicles yet)
    struct LanePath {
        traffic::LaneId id = 0;
        std::array<double, 2> a{}, b{};
        bool connector = false;
    };
    std::vector<LanePath> lane_paths;
    std::vector<traffic::LaneId> entry_lanes;  // lanes on which vehicles may be introduced at the district's edge
    multi_floor_nav::MultiFloorNavGraph interior_nav{0.5, 0.35};
};

inline constexpr double lane_offset = 1.75;     // driving lane centre from the road's centreline
inline constexpr double road_half = 5.5;        // asphalt half width: two 3.5 m driving lanes and a 2 m parking bay on each side
inline constexpr double row_half = 9.0;         // the whole corridor: asphalt plus 3.5 m sidewalks
inline constexpr double corner_offset = 7.25;   // the sidewalk centreline from the road's centreline
inline constexpr double stop_distance = 9.5;    // from an intersection's centre to the stop line (before the crosswalk)

// Builds the district. Deterministic in `seed`.
[[nodiscard]] District build_district(std::uint32_t seed);

// Shortest path over the pedestrian network (node indices, inclusive of both ends); empty if there is none. `walk_crossings` false forbids crossing edges.
[[nodiscard]] std::vector<std::uint32_t> ped_path(const District& d, std::uint32_t from, std::uint32_t to, bool walk_crossings = true);
// The pedestrian node nearest to a point (layer 0).
[[nodiscard]] std::uint32_t nearest_ped_node(const District& d, double x, double y);

// Height of the walkable surface at a point outdoors (sidewalk, promenade, pier, road), and whether the point is over water.
[[nodiscard]] double ground_height(const District& d, double x, double y);
[[nodiscard]] bool over_water(const District& d, double x, double y);

} // namespace saige::alive
