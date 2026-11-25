#include "district.hpp"
#include "district_detail.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace saige::alive {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double avenue_x[3] = {-90.0, 0.0, 90.0};
constexpr double street_y[3] = {-60.0, 0.0, 60.0};
constexpr double boundary_run = 80.0;  // how far a boundary stub runs beyond its intersection

std::array<double, 2> right_of(double dx, double dy) { return {dy, -dx}; }

struct LaneBuilder {
    District& d;
    traffic::LaneId next = 1;
    traffic::LaneId add(std::array<double, 2> a, std::array<double, 2> b, double speed, bool connector, traffic::IntersectionId end_intersection) {
        traffic::LaneSegment lane;
        lane.id = next++;
        lane.start = {a[0], a[1]};
        lane.end = {b[0], b[1]};
        lane.speed_limit_mps = speed;
        lane.end_intersection = end_intersection;
        d.traffic.add_lane(lane);
        d.lane_paths.push_back({lane.id, a, b, connector});
        return lane.id;
    }
};

const char* const cafe_names[] = {"Harbor Light Café", "Saltwater Café", "Pier Nine Café"};
const char* const shop_names[] = {"Corner Market", "Marlow Provisions", "Tidewater Mini-Mart", "Bayside Grocer"};
const char* const bakery_names[] = {"Gull's Rest Bakery", "Morning Tide Bakery"};
const char* const bar_names[] = {"The Lamplighter", "Anchor & Compass", "The Brass Gull"};
const char* const pharmacy_names[] = {"Seaview Pharmacy", "Bayside Chemist"};
const char* const hotel_names[] = {"Hotel Marlow", "The Wharf Inn", "Harborview Hotel"};
const char* const apartment_names[] = {"Gull Court", "Kestrel House", "Wren Terrace", "Heron Lodge", "Tern Row", "Lantern Court", "Cormorant Place", "Plover House"};
const char* const office_names[] = {"Harbor Trading Co.", "Marlow & Sons", "Coastline Insurance", "Port Authority Annex"};

bool contains_rect(const std::vector<Rect>& v, double x, double y) {
    for (const Rect& r : v) {
        if (r.contains(x, y)) return true;
    }
    return false;
}

} // namespace

const char* kind_name(BuildingKind k) {
    switch (k) {
    case BuildingKind::Cafe: return "cafe";
    case BuildingKind::Shop: return "shop";
    case BuildingKind::Bakery: return "bakery";
    case BuildingKind::Bar: return "bar";
    case BuildingKind::Pharmacy: return "pharmacy";
    case BuildingKind::Apartments: return "apartments";
    case BuildingKind::Offices: return "offices";
    case BuildingKind::Hotel: return "hotel";
    case BuildingKind::Warehouse: return "warehouse";
    case BuildingKind::FireStation: return "fire station";
    case BuildingKind::Count: break;
    }
    return "?";
}

bool is_commercial(BuildingKind k) {
    return k == BuildingKind::Cafe || k == BuildingKind::Shop || k == BuildingKind::Bakery || k == BuildingKind::Bar || k == BuildingKind::Pharmacy || k == BuildingKind::Hotel || k == BuildingKind::Offices;
}

double ground_height(const District& d, double x, double y) {
    for (const Pier& p : d.piers) {
        if (p.deck.contains(x, y)) return p.z;
    }
    if (y > d.quay_y) return d.sea_level;
    if (d.promenade.contains(x, y)) return 0.15;
    // Sidewalks sit 0.15 m above the asphalt.
    for (const Rect& r : d.sidewalk) {
        if (r.contains(x, y)) return 0.15;
    }
    return 0.0;
}

bool over_water(const District& d, double x, double y) {
    if (y <= d.quay_y) return false;
    for (const Pier& p : d.piers) {
        if (p.deck.contains(x, y)) return false;
    }
    return true;
}

District build_district(std::uint32_t seed) {
    District d;
    d.seed = seed;
    Rng rng(seed);
    d.bounds = {-135.0, -105.0, 135.0, 78.0};
    d.quay_y = 78.0;
    d.promenade = {-135.0, 69.0, 135.0, 78.0};

    // ---- intersections, lanes, signals ------------------------------------------------------------------------------------------------------------------------------------------------------------
    LaneBuilder lanes{d};
    auto inter_index = [](int ix, int iy) { return static_cast<std::size_t>(ix + 3 * iy); };
    for (int iy = 0; iy < 3; ++iy) {
        for (int ix = 0; ix < 3; ++ix) {
            Intersection in;
            in.id = static_cast<std::uint32_t>(inter_index(ix, iy)) + 1;
            in.x = avenue_x[ix];
            in.y = street_y[iy];
            in.has_leg = {true, true, iy < 2, true};  // the Harbor Road intersections have no north leg: the avenues end at the promenade
            d.intersections.push_back(in);
        }
    }
    // Segment lanes. A neighbouring intersection exists for East/West/North/South legs inside the grid.
    auto neighbour = [&](int ix, int iy, Leg l) -> int {
        const auto dir = leg_dir(l);
        const int jx = ix + static_cast<int>(dir[0]);
        const int jy = iy + static_cast<int>(dir[1]);
        if (jx < 0 || jx > 2 || jy < 0 || jy > 2) return -1;
        return static_cast<int>(inter_index(jx, jy));
    };
    auto opposite = [](Leg l) { return static_cast<Leg>((static_cast<int>(l) + 2) % 4); };
    // 1) lanes between adjacent intersections, 2) boundary stubs.
    for (int iy = 0; iy < 3; ++iy) {
        for (int ix = 0; ix < 3; ++ix) {
            Intersection& I = d.intersections[inter_index(ix, iy)];
            for (int li = 0; li < 4; ++li) {
                const Leg leg = static_cast<Leg>(li);
                if (!I.has_leg[static_cast<std::size_t>(li)]) continue;
                const auto dir = leg_dir(leg);
                const auto rt = right_of(dir[0], dir[1]);
                const int nb = neighbour(ix, iy, leg);
                // Outbound lane: leaves I along `dir`.
                const std::array<double, 2> a{I.x + dir[0] * stop_distance + rt[0] * lane_offset, I.y + dir[1] * stop_distance + rt[1] * lane_offset};
                if (nb >= 0) {
                    Intersection& J = d.intersections[static_cast<std::size_t>(nb)];
                    const std::array<double, 2> b{J.x - dir[0] * stop_distance + rt[0] * lane_offset, J.y - dir[1] * stop_distance + rt[1] * lane_offset};
                    const traffic::LaneId id = lanes.add(a, b, 11.1, false, J.id);
                    I.outbound[static_cast<std::size_t>(li)] = id;
                    J.inbound[static_cast<std::size_t>(static_cast<int>(opposite(leg)))] = id;
                } else {
                    const std::array<double, 2> b{I.x + dir[0] * boundary_run + rt[0] * lane_offset, I.y + dir[1] * boundary_run + rt[1] * lane_offset};
                    I.outbound[static_cast<std::size_t>(li)] = lanes.add(a, b, 11.1, false, traffic::kInvalidIntersectionId);
                    // Inbound lane from the boundary into I.
                    const auto rin = right_of(-dir[0], -dir[1]);
                    const std::array<double, 2> c{I.x + dir[0] * boundary_run + rin[0] * lane_offset, I.y + dir[1] * boundary_run + rin[1] * lane_offset};
                    const std::array<double, 2> e{I.x + dir[0] * stop_distance + rin[0] * lane_offset, I.y + dir[1] * stop_distance + rin[1] * lane_offset};
                    I.inbound[static_cast<std::size_t>(li)] = lanes.add(c, e, 11.1, false, I.id);
                    d.entry_lanes.push_back(I.inbound[static_cast<std::size_t>(li)]);
                }
            }
        }
    }
    // Connectors through each intersection, and the lane successor links. Lane records are immutable once added to the traffic simulation, so the successor lists are collected first and the lanes rebuilt.
    struct Link {
        traffic::LaneId from, to;
    };
    std::vector<Link> links;
    for (Intersection& I : d.intersections) {
        for (int di = 0; di < 4; ++di) {
            if (!I.has_leg[static_cast<std::size_t>(di)]) continue;
            for (int dout = 0; dout < 4; ++dout) {
                if (dout == di || !I.has_leg[static_cast<std::size_t>(dout)]) continue;
                const auto leg_in = leg_dir(static_cast<Leg>(di));
                const auto travel_in = std::array<double, 2>{-leg_in[0], -leg_in[1]};
                const auto r_in = right_of(travel_in[0], travel_in[1]);
                const auto leg_out = leg_dir(static_cast<Leg>(dout));
                const auto r_out = right_of(leg_out[0], leg_out[1]);
                const std::array<double, 2> a{I.x + leg_in[0] * stop_distance + r_in[0] * lane_offset, I.y + leg_in[1] * stop_distance + r_in[1] * lane_offset};
                const std::array<double, 2> b{I.x + leg_out[0] * stop_distance + r_out[0] * lane_offset, I.y + leg_out[1] * stop_distance + r_out[1] * lane_offset};
                const traffic::LaneId c = lanes.add(a, b, 6.0, true, traffic::kInvalidIntersectionId);
                links.push_back({I.inbound[static_cast<std::size_t>(di)], c});
                links.push_back({c, I.outbound[static_cast<std::size_t>(dout)]});
            }
        }
    }
    // Rebuild the traffic simulation with the successors (the simulation stores lanes by value).
    {
        std::vector<traffic::LaneSegment> built;
        // Recreate from lane_paths with the right speed and end intersection.
        std::map<traffic::LaneId, std::vector<traffic::LaneId>> succ;
        for (const Link& l : links) succ[l.from].push_back(l.to);
        std::map<traffic::LaneId, traffic::IntersectionId> end_of;
        for (const Intersection& I : d.intersections) {
            for (int li = 0; li < 4; ++li) {
                if (I.inbound[static_cast<std::size_t>(li)] != 0) end_of[I.inbound[static_cast<std::size_t>(li)]] = I.id;
            }
        }
        d.traffic = traffic::TrafficSimulation();
        for (const District::LanePath& lp : d.lane_paths) {
            traffic::LaneSegment lane;
            lane.id = lp.id;
            lane.start = {lp.a[0], lp.a[1]};
            lane.end = {lp.b[0], lp.b[1]};
            lane.speed_limit_mps = lp.connector ? 6.0 : 11.1;
            const auto e = end_of.find(lp.id);
            lane.end_intersection = e != end_of.end() ? e->second : traffic::kInvalidIntersectionId;
            const auto s = succ.find(lp.id);
            if (s != succ.end()) lane.successor_lanes = s->second;
            d.traffic.add_lane(lane);
        }
    }
    // Signals: split phasing (one approach at a time) and a pedestrian phase with every approach red.
    for (const Intersection& I : d.intersections) {
        traffic::IntersectionController c;
        c.id = I.id;
        for (int li = 0; li < 4; ++li) {
            if (!I.has_leg[static_cast<std::size_t>(li)] || I.inbound[static_cast<std::size_t>(li)] == 0) continue;
            traffic::SignalPhase p;
            p.green_lanes = {I.inbound[static_cast<std::size_t>(li)]};
            p.green_duration_sec = 9.0;
            p.yellow_duration_sec = 3.0;
            p.all_red_duration_sec = 2.0;
            c.phases.push_back(p);
        }
        traffic::SignalPhase walk;
        walk.green_duration_sec = 12.0;  // no lane is green: this is the pedestrian phase
        walk.yellow_duration_sec = 3.0;
        walk.all_red_duration_sec = 1.0;
        c.phases.push_back(walk);
        // Stagger the cycles so the district's signals do not all change together.
        c.active_phase_idx = static_cast<std::size_t>(I.id) % c.phases.size();
        c.phase_timer_sec = 1.7 * static_cast<double>(I.id % 5);
        d.traffic.add_intersection(c);
    }

    // ---- roadway and sidewalk rectangles ------------------------------------------------------------------------------------------------------------------------------------------------------------
    const double x_min = d.bounds.x0 - 20.0, x_max = d.bounds.x1 + 20.0;
    for (const double ax : avenue_x) {
        d.roadway.push_back({ax - road_half, d.bounds.y0 - 20.0, ax + road_half, street_y[2] + road_half});
        d.sidewalk.push_back({ax - row_half, d.bounds.y0 - 20.0, ax - road_half, street_y[2] + row_half});
        d.sidewalk.push_back({ax + road_half, d.bounds.y0 - 20.0, ax + row_half, street_y[2] + row_half});
    }
    for (const double sy : street_y) {
        d.roadway.push_back({x_min, sy - road_half, x_max, sy + road_half});
        d.sidewalk.push_back({x_min, sy - row_half, x_max, sy - road_half});
        d.sidewalk.push_back({x_min, sy + road_half, x_max, sy + row_half});
    }

    // ---- crosswalks --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    std::uint32_t next_crosswalk = 1, next_zone = 1;
    for (Intersection& I : d.intersections) {
        for (int li = 0; li < 4; ++li) {
            if (!I.has_leg[static_cast<std::size_t>(li)]) continue;
            const auto dir = leg_dir(static_cast<Leg>(li));
            const auto perp = std::array<double, 2>{-dir[1], dir[0]};
            Crosswalk c;
            c.id = next_crosswalk++;
            c.intersection = I.id;
            c.leg = static_cast<Leg>(li);
            const double cx = I.x + dir[0] * corner_offset, cy = I.y + dir[1] * corner_offset;
            const double half_t = 1.3;
            // The strip is 2.6 m thick along the leg and as wide as the roadway across it.
            const double ex = std::abs(dir[0]) * half_t + std::abs(perp[0]) * road_half;
            const double ey = std::abs(dir[1]) * half_t + std::abs(perp[1]) * road_half;
            c.strip = {cx - ex, cy - ey, cx + ex, cy + ey};
            c.a = {cx + perp[0] * corner_offset, cy + perp[1] * corner_offset};
            c.b = {cx - perp[0] * corner_offset, cy - perp[1] * corner_offset};
            I.crosswalk[static_cast<std::size_t>(li)] = c.id;
            d.crosswalks.push_back(c);
        }
    }
    // Traffic zones: every lane (connectors cross the strips) whose path passes through a strip.
    for (Crosswalk& cw : d.crosswalks) {
        for (const District::LanePath& lp : d.lane_paths) {
            const double dx = lp.b[0] - lp.a[0], dy = lp.b[1] - lp.a[1];
            const double len = std::hypot(dx, dy);
            double s_in = -1.0, s_out = -1.0;
            for (double s = 0.0; s <= len; s += 0.1) {
                const double px = lp.a[0] + dx * s / len, py = lp.a[1] + dy * s / len;
                if (cw.strip.contains(px, py)) {
                    if (s_in < 0.0) s_in = s;
                    s_out = s;
                }
            }
            if (s_in < 0.0) continue;
            traffic::CrosswalkZone z;
            z.id = next_zone++;
            z.lane_id = lp.id;
            z.s_start_m = std::max(0.0, s_in - 0.5);
            z.s_end_m = s_out + 0.5;
            d.traffic.add_crosswalk(z);
            cw.traffic_zones.push_back(z.id);
        }
    }

    // ---- blocks, lots, buildings --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    const double bx[5] = {-135.0, -81.0 - 18.0 + 18.0, 0, 0, 0};  // placeholder to keep the arrays obvious below
    (void)bx;
    struct Block {
        Rect r;
        int col, row;
    };
    std::vector<Block> blocks;
    const double xs[4][2] = {{-135.0, -99.0}, {-81.0, -9.0}, {9.0, 81.0}, {99.0, 135.0}};
    const double ys[3][2] = {{-105.0, -69.0}, {-51.0, -9.0}, {9.0, 51.0}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) blocks.push_back({{xs[col][0], ys[row][0], xs[col][1], ys[row][1]}, col, row});
    }
    std::uint32_t next_building = 1;
    int cafe_i = 0, shop_i = 0, bakery_i = 0, bar_i = 0, pharm_i = 0, hotel_i = 0, apt_i = 0, off_i = 0;
    auto name_for = [&](BuildingKind k) -> std::string {
        switch (k) {
        case BuildingKind::Cafe: return cafe_names[cafe_i++ % 3];
        case BuildingKind::Shop: return shop_names[shop_i++ % 4];
        case BuildingKind::Bakery: return bakery_names[bakery_i++ % 2];
        case BuildingKind::Bar: return bar_names[bar_i++ % 3];
        case BuildingKind::Pharmacy: return pharmacy_names[pharm_i++ % 2];
        case BuildingKind::Hotel: return hotel_names[hotel_i++ % 3];
        case BuildingKind::Apartments: return apartment_names[apt_i++ % 8];
        case BuildingKind::Offices: return office_names[off_i++ % 4];
        case BuildingKind::Warehouse: return "Harbor Warehouse";
        case BuildingKind::FireStation: return "Marlow Fire Station";
        case BuildingKind::Count: break;
        }
        return "";
    };
    // Kinds by block and side.
    auto kind_for = [&](const Block& b, Leg side, Rng& r) -> BuildingKind {
        const bool waterfront_side = (b.row == 2 && side == Leg::North);
        if (b.row == 2) {
            if (waterfront_side) {
                const int k = r.pick(10);
                if (k < 3) return BuildingKind::Cafe;
                if (k < 5) return BuildingKind::Bar;
                if (k < 7) return BuildingKind::Hotel;
                return BuildingKind::Shop;
            }
            if (side == Leg::South) {
                const int k = r.pick(10);
                if (k < 3) return BuildingKind::Shop;
                if (k < 5) return BuildingKind::Bakery;
                if (k < 6) return BuildingKind::Pharmacy;
                if (k < 8) return BuildingKind::Offices;
                return BuildingKind::Apartments;
            }
            return r.pick(3) == 0 ? BuildingKind::Offices : BuildingKind::Apartments;
        }
        if (b.row == 1) {
            if (side == Leg::North) {
                const int k = r.pick(10);
                if (k < 4) return BuildingKind::Shop;
                if (k < 6) return BuildingKind::Bakery;
                return BuildingKind::Offices;
            }
            return r.pick(4) == 0 ? BuildingKind::Offices : BuildingKind::Apartments;
        }
        return BuildingKind::Apartments;
    };
    const Rect park_block = {9.0, -105.0, 81.0, -69.0};
    d.park.push_back(park_block);
    const Rect station_block = {-81.0, -105.0, -9.0, -69.0};
    for (const Block& b : blocks) {
        if (b.r.x0 == park_block.x0 && b.r.y0 == park_block.y0) continue;  // the park
        const double depth_base = 13.0;
        for (int side_i = 0; side_i < 4; ++side_i) {
            const Leg side = static_cast<Leg>(side_i);
            const bool horizontal = (side == Leg::South || side == Leg::North);
            // The run of the side. South and North take the full width; East and West the part between them.
            double t0 = horizontal ? b.r.x0 : b.r.y0 + depth_base;
            double t1 = horizontal ? b.r.x1 : b.r.y1 - depth_base;
            if (t1 - t0 < 6.0) continue;
            double t = t0;
            while (t < t1 - 1e-6) {
                double w = rng.range(9.0, 15.0);
                if (t1 - (t + w) < 8.0) w = t1 - t;  // absorb a short remainder
                BuildingKind kind = kind_for(b, side, rng);
                // At most depth_base, so the east and west rows (which start depth_base in from the block's edge) never run into the south and north rows.
                const double depth = (is_commercial(kind) ? depth_base : depth_base - 1.0) - rng.range(0.0, 1.5);
                Building bd;
                bd.id = next_building++;
                bd.kind = kind;
                bd.front = side;
                bd.floors = 3;
                switch (kind) {
                case BuildingKind::Apartments: bd.floors = 3 + rng.pick(3); break;
                case BuildingKind::Offices: bd.floors = 3 + rng.pick(3); break;
                case BuildingKind::Hotel: bd.floors = 4 + rng.pick(2); break;
                default: bd.floors = 2 + rng.pick(3); break;
                }
                if (b.r.x0 == station_block.x0 && b.r.y0 == station_block.y0 && side == Leg::North && t > station_block.x0 + 20 && t < station_block.x0 + 36) {
                    bd.kind = BuildingKind::FireStation;
                    bd.floors = 2;
                    w = std::min(w + 6.0, t1 - t);
                }
                if (horizontal) {
                    bd.footprint = side == Leg::South ? Rect{t, b.r.y0, t + w, b.r.y0 + depth} : Rect{t, b.r.y1 - depth, t + w, b.r.y1};
                    bd.door_x = t + w * rng.range(0.35, 0.65);
                    bd.door_y = side == Leg::South ? b.r.y0 : b.r.y1;
                } else {
                    bd.footprint = side == Leg::West ? Rect{b.r.x0, t, b.r.x0 + depth, t + w} : Rect{b.r.x1 - depth, t, b.r.x1, t + w};
                    bd.door_y = t + w * rng.range(0.35, 0.65);
                    bd.door_x = side == Leg::West ? b.r.x0 : b.r.x1;
                }
                bd.palette = rng.pick(6);
                bd.name = name_for(bd.kind);
                d.buildings.push_back(bd);
                t += w;
            }
        }
    }

    // ---- the waterfront: promenade, quay, piers, boats ----------------------------------------------------------------------------------------------------------------------------------------------
    d.sidewalk.push_back(d.promenade);
    d.piers.push_back({{-48.0, 78.0, -42.0, 108.0}, 0.25});
    d.piers.push_back({{34.0, 78.0, 44.0, 102.0}, 0.25});
    d.boats.push_back({-52.0, 96.0, kPi * 0.5, 4.4, 1.7, false});
    d.boats.push_back({-38.0, 90.0, kPi * 0.5, 5.2, 1.9, false});
    d.boats.push_back({30.0, 92.0, kPi * 0.5, 4.0, 1.6, false});
    d.boats.push_back({48.0, 88.0, kPi * 0.5, 6.0, 2.2, false});
    d.boats.push_back({60.0, 150.0, kPi, 7.0, 2.6, true});

    // ---- parking ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // Bays on both sides of the east-west streets between the avenues (kerbside, 2 m wide, parked parallel to the kerb), away from the intersections.
    for (const double sy : street_y) {
        for (int seg = 0; seg < 4; ++seg) {
            const double xa = (seg == 0 ? -135.0 : seg == 1 ? -81.0 : seg == 2 ? 9.0 : 99.0) + 3.0;
            const double xb = (seg == 0 ? -99.0 : seg == 1 ? -9.0 : seg == 2 ? 81.0 : 135.0) - 3.0;
            for (const int sgn : {-1, 1}) {
                if (sy > 50.0 && sgn > 0) continue;  // the harbor side of Harbor Road has the promenade, no bays
                for (double x = xa + 3.0; x < xb - 3.0; x += 6.4) {
                    if (rng.unit() < 0.55) {
                        d.parking.push_back({x, sy + sgn * 4.5, sgn > 0 ? 0.0 : kPi, false});
                    }
                }
            }
        }
    }
    build_interiors(d, rng);
    build_props(d, rng);
    build_ped_network(d);
    build_venues(d, rng);
    build_collision(d);
    return d;
}

} // namespace saige::alive
