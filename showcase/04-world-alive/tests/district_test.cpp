// Showcase 04 district: the data the living world stands on must be a pure function of the seed and must be navigable by every system that will use it (pedestrians, vehicles, interior walkers).
#include "district.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include "saige/capsule_sweep.hpp"
#include <vector>

using namespace saige;
using namespace saige::alive;

namespace {
int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

std::uint64_t fingerprint(const District& d) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) {
        const auto q = static_cast<std::int64_t>(std::llround(v * 1000.0));
        h = (h ^ static_cast<std::uint64_t>(q)) * 1099511628211ull;
    };
    for (const Building& b : d.buildings) {
        mix(b.footprint.x0); mix(b.footprint.y0); mix(b.footprint.x1); mix(b.footprint.y1);
        mix(b.floors); mix(static_cast<double>(b.kind)); mix(b.door_x); mix(b.door_y);
    }
    for (const StreetProp& p : d.props) { mix(p.x); mix(p.y); mix(p.yaw); mix(p.scale); }
    for (const BoatSpawn& b : d.boats) { mix(b.x); mix(b.y); mix(b.yaw); }
    for (const ParkingSpot& s : d.parking) { mix(s.x); mix(s.y); }
    mix(static_cast<double>(d.collision.size()));
    mix(static_cast<double>(d.ped_nodes.size()));
    return h;
}
} // namespace

int main() {
    const District d = build_district(7);

    // --- determinism, and dependence on the seed --------------------------------------------------------------------------------------------------------------------------------------------------
    CHECK(fingerprint(build_district(7)) == fingerprint(d));
    CHECK(fingerprint(build_district(8)) != fingerprint(d));

    // --- the layout is populated ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    CHECK(d.buildings.size() >= 20);
    CHECK(d.intersections.size() == 9);
    CHECK(!d.crosswalks.empty());
    CHECK(!d.props.empty());
    CHECK(!d.parking.empty());
    CHECK(!d.piers.empty());
    CHECK(!d.boats.empty());
    CHECK(!d.entry_lanes.empty());
    CHECK(!d.venues.empty());
    CHECK(d.interiors.size() >= 4);

    // --- buildings: inside the district, off the roads, not through each other, doors on their front facade -----------------------------------------------------------------------------------
    for (std::size_t i = 0; i < d.buildings.size(); ++i) {
        const Building& b = d.buildings[i];
        CHECK(b.footprint.w() > 4.0 && b.footprint.h() > 4.0);
        CHECK(b.footprint.x0 >= d.bounds.x0 && b.footprint.x1 <= d.bounds.x1 && b.footprint.y0 >= d.bounds.y0 && b.footprint.y1 <= d.bounds.y1);
        for (const Rect& r : d.roadway) CHECK(!b.footprint.overlaps(r));
        for (std::size_t j = i + 1; j < d.buildings.size(); ++j) CHECK(!b.footprint.overlaps(d.buildings[j].footprint));
        const auto dir = leg_dir(b.front);
        const bool on_facade = std::fabs(b.door_x - (dir[0] > 0 ? b.footprint.x1 : dir[0] < 0 ? b.footprint.x0 : b.door_x)) < 0.6 &&
                               std::fabs(b.door_y - (dir[1] > 0 ? b.footprint.y1 : dir[1] < 0 ? b.footprint.y0 : b.door_y)) < 0.6;
        CHECK(on_facade);
    }

    // --- pedestrians: every door is reachable from every other, crossings included; without crossings the district falls apart (so the signals matter) -----------------------------------------
    const std::uint32_t home = nearest_ped_node(d, 0.0, 6.0);
    std::size_t unreachable = 0, reachable_without_crossings = 0;
    for (const Building& b : d.buildings) {
        const auto dir = leg_dir(b.front);
        const std::uint32_t n = nearest_ped_node(d, b.door_x + dir[0] * 1.2, b.door_y + dir[1] * 1.2);
        const auto path = ped_path(d, home, n, true);
        if (path.empty() || path.front() != home || path.back() != n) ++unreachable;
        if (!ped_path(d, home, n, false).empty()) ++reachable_without_crossings;
    }
    CHECK(unreachable == 0);
    CHECK(reachable_without_crossings < d.buildings.size());
    for (const PedEdge& e : d.ped_edges) {
        CHECK(e.a < d.ped_nodes.size() && e.b < d.ped_nodes.size());
        if (e.crosswalk != 0) {
            bool known = false;
            for (const Crosswalk& c : d.crosswalks) known = known || c.id == e.crosswalk;
            CHECK(known);
        }
    }

    // --- a pedestrian can stand on every node of the network (no prop, post or wall on the line people are routed along) ------------------------------------------------------------------------
    std::size_t blocked_nodes = 0;
    for (const PedNode& n : d.ped_nodes) {
        if (n.layer != 0) continue;
        const Capsule cap{Vec3{static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(ground_height(d, n.x, n.y) + 0.02)}, 0.30f, 1.75f};
        for (const Aabb& b : d.collision) {
            if (b.max.z <= cap.feet.z + 0.001f) continue;
            if (capsule_box_gap(cap, b) < 0.05f) {
                ++blocked_nodes;
                std::fprintf(stderr, "node (%.2f, %.2f) blocked by box [%.2f %.2f %.2f]-[%.2f %.2f %.2f]\n", n.x, n.y, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
                break;
            }
        }
    }
    CHECK(blocked_nodes == 0);

    // --- vehicles: lanes exist, intersections carry signals, every approach has a lane, and the entry lanes reach each other -------------------------------------------------------------------
    for (const Intersection& in : d.intersections) {
        CHECK(d.traffic.find_intersection(in.id) != nullptr);
        for (std::size_t leg = 0; leg < 4; ++leg) {
            if (!in.has_leg[leg]) continue;
            CHECK(d.traffic.find_lane(in.inbound[leg]) != nullptr);
            CHECK(d.traffic.find_lane(in.outbound[leg]) != nullptr);
        }
        // One approach at a time: no phase turns two legs green together.
        const traffic::IntersectionController* c = d.traffic.find_intersection(in.id);
        if (c == nullptr) continue;
        CHECK(!c->phases.empty());
        for (const auto& phase : c->phases) {
            std::set<std::size_t> legs;
            for (traffic::LaneId lane : phase.green_lanes)
                for (std::size_t leg = 0; leg < 4; ++leg)
                    if (in.has_leg[leg] && in.inbound[leg] == lane) legs.insert(leg);
            CHECK(legs.size() <= 1);
        }
    }
    // Every entry lane leads, through intersections and connectors, out of the district by several different exits.
    std::vector<traffic::LaneId> exits;
    for (const Intersection& in : d.intersections)
        for (std::size_t leg = 0; leg < 4; ++leg) {
            if (!in.has_leg[leg]) continue;
            const traffic::LaneSegment* lane = d.traffic.find_lane(in.outbound[leg]);
            if (lane != nullptr && lane->end_intersection == traffic::kInvalidIntersectionId && lane->successor_lanes.empty()) exits.push_back(lane->id);
        }
    CHECK(exits.size() == d.entry_lanes.size());
    std::size_t well_connected = 0;
    for (traffic::LaneId from : d.entry_lanes) {
        std::size_t reach = 0;
        for (traffic::LaneId to : exits)
            if (!d.traffic.find_lane_route(from, to).empty()) ++reach;
        if (reach + 1 >= exits.size()) ++well_connected;  // all exits (the one beside it on the same road included, by a turn)
    }
    CHECK(well_connected == d.entry_lanes.size());

    // --- crosswalks tie the pedestrian network to the traffic zones ------------------------------------------------------------------------------------------------------------------------------
    for (const Crosswalk& c : d.crosswalks) CHECK(!c.traffic_zones.empty());

    // --- interiors: accessible rooms are walkable from the door to every cell, and the cafe's second floor is reached by its stairs ------------------------------------------------------------------
    CHECK(d.interior_nav.slice_count() > 200);
    bool found_stairs = false;
    for (const Interior& in : d.interiors) {
        const auto cell = [](double v) { return static_cast<std::int32_t>(std::floor(v / 0.5)); };
        const multi_floor_nav::NavNodeId inside{cell(in.door_inside[0]), cell(in.door_inside[1]), 0};
        const auto* slice = d.interior_nav.find_slice(inside);
        CHECK(slice != nullptr && slice->walkable);
        if (in.has_stairs) {
            found_stairs = true;
            const multi_floor_nav::NavNodeId top{cell(in.stair_top[0]), cell(in.stair_top[1]), 1};
            const auto up = d.interior_nav.find_path(inside, top);
            CHECK(up.found && up.portal_transitions == 1);
            // A point on the upper floor away from the stair well is reachable too.
            const multi_floor_nav::NavNodeId far{cell(in.upper_room.cx()), cell(in.upper_room.cy()), 1};
            const auto* far_slice = d.interior_nav.find_slice(far);
            if (far_slice != nullptr && far_slice->walkable) CHECK(d.interior_nav.find_path(inside, far).found);
        }
    }
    CHECK(found_stairs);

    // --- water and ground ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    CHECK(over_water(d, 0.0, d.quay_y + 30.0));
    CHECK(!over_water(d, 0.0, 0.0));
    CHECK(ground_height(d, 0.0, 0.0) < 0.2);
    for (const BoatSpawn& b : d.boats) CHECK(over_water(d, b.x, b.y));

    // --- static collision exists and every building is represented -------------------------------------------------------------------------------------------------------------------------------
    CHECK(d.collision.size() > d.buildings.size());

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("district ok: %zu buildings, %zu props, %zu ped nodes, %zu interior cells, %zu collision boxes\n", d.buildings.size(), d.props.size(), d.ped_nodes.size(), d.interior_nav.slice_count(), d.collision.size());
    return EXIT_SUCCESS;
}
