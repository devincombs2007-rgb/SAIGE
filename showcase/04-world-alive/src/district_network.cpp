#include "district_detail.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>

namespace saige::alive {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double avenue_x[3] = {-90.0, 0.0, 90.0};
constexpr double street_y[3] = {-60.0, 0.0, 60.0};

std::uint32_t add_node(District& d, double x, double y, std::uint8_t layer = 0) {
    d.ped_nodes.push_back({x, y, layer, 0});
    return static_cast<std::uint32_t>(d.ped_nodes.size() - 1);
}
void add_edge(District& d, std::uint32_t a, std::uint32_t b, std::uint32_t crosswalk = 0) {
    if (a == b) return;
    d.ped_edges.push_back({a, b, crosswalk});
}

} // namespace

void build_ped_network(District& d) {
    // Sidewalk centrelines: horizontal at y = street +- corner_offset, vertical at x = avenue +- corner_offset.
    struct Line {
        bool horizontal;
        double fixed;
        std::vector<std::pair<double, std::uint32_t>> nodes;  // (parameter along the line, node)
    };
    std::vector<Line> lines;
    for (const double sy : street_y) {
        for (const int sgn : {-1, 1}) lines.push_back({true, sy + sgn * corner_offset, {}});
    }
    for (const double ax : avenue_x) {
        for (const int sgn : {-1, 1}) lines.push_back({false, ax + sgn * corner_offset, {}});
    }
    auto find_line = [&](bool horizontal, double fixed) -> Line* {
        for (Line& l : lines) {
            if (l.horizontal == horizontal && std::abs(l.fixed - fixed) < 1e-6) return &l;
        }
        return nullptr;
    };
    // Corner nodes where the lines cross, shared by both.
    std::map<std::pair<long, long>, std::uint32_t> corner;
    auto key = [](double x, double y) { return std::make_pair(std::lround(x * 100), std::lround(y * 100)); };
    for (const double sy : street_y) {
        for (const double ax : avenue_x) {
            for (const int sx : {-1, 1}) {
                for (const int sgn : {-1, 1}) {
                    const double x = ax + sx * corner_offset, y = sy + sgn * corner_offset;
                    const std::uint32_t n = add_node(d, x, y);
                    corner[key(x, y)] = n;
                    find_line(true, y)->nodes.push_back({x, n});
                    find_line(false, x)->nodes.push_back({y, n});
                }
            }
        }
    }
    // Line ends beyond the outermost intersections (the district boundary).
    for (Line& l : lines) {
        if (l.horizontal) {
            l.nodes.push_back({d.bounds.x0, add_node(d, d.bounds.x0, l.fixed)});
            l.nodes.push_back({d.bounds.x1, add_node(d, d.bounds.x1, l.fixed)});
        } else {
            l.nodes.push_back({d.bounds.y0, add_node(d, l.fixed, d.bounds.y0)});
        }
    }
    // Door nodes on the sidewalk in front of each building, and on the promenade.
    std::map<std::uint32_t, std::uint32_t> door_node;  // building id -> node
    for (const Building& b : d.buildings) {
        const auto dir = leg_dir(b.front);
        const double x = b.door_x + dir[0] * 1.75, y = b.door_y + dir[1] * 1.75;
        const bool horizontal = (b.front == Leg::South || b.front == Leg::North);
        Line* l = find_line(horizontal, horizontal ? y : x);
        if (l == nullptr) continue;  // a lot whose street is not on the network (the boundary blocks)
        const std::uint32_t n = add_node(d, x, y);
        l->nodes.push_back({horizontal ? x : y, n});
        door_node[b.id] = n;
    }
    // Sort each line and chain consecutive nodes; an edge that spans a crosswalk is a crossing.
    auto crosswalk_between = [&](const Line& l, double t0, double t1) -> std::uint32_t {
        for (const Crosswalk& c : d.crosswalks) {
            // The crosswalk runs perpendicular to its leg: for a horizontal line the crosswalks of N/S legs lie across it.
            const bool across_horizontal = (c.leg == Leg::North || c.leg == Leg::South);
            if (across_horizontal != l.horizontal) continue;
            const double cy = 0.5 * (c.strip.y0 + c.strip.y1), cx = 0.5 * (c.strip.x0 + c.strip.x1);
            if (l.horizontal) {
                if (std::abs(cy - l.fixed) > 0.05) continue;
                if (std::min(t0, t1) < cx && cx < std::max(t0, t1) && std::abs(t1 - t0) < 2.0 * corner_offset + 0.5) return c.id;
            } else {
                if (std::abs(cx - l.fixed) > 0.05) continue;
                if (std::min(t0, t1) < cy && cy < std::max(t0, t1) && std::abs(t1 - t0) < 2.0 * corner_offset + 0.5) return c.id;
            }
        }
        return 0;
    };
    for (Line& l : lines) {
        std::sort(l.nodes.begin(), l.nodes.end());
        for (std::size_t i = 0; i + 1 < l.nodes.size(); ++i) {
            const std::uint32_t cw = crosswalk_between(l, l.nodes[i].first, l.nodes[i + 1].first);
            add_edge(d, l.nodes[i].second, l.nodes[i + 1].second, cw);
        }
    }
    // The promenade: a line along the water, linked to the sidewalk corners at the avenues and every 15 m, and the piers.
    const double py = 0.5 * (d.promenade.y0 + d.promenade.y1);
    std::vector<std::pair<double, std::uint32_t>> prom;
    for (double x = d.bounds.x0; x <= d.bounds.x1 + 1e-6; x += 15.0) prom.push_back({x, add_node(d, x, py)});
    for (const double ax : avenue_x) {
        for (const int sx : {-1, 1}) {
            const double x = ax + sx * corner_offset;
            const std::uint32_t top = add_node(d, x, py);
            prom.push_back({x, top});
            const auto c = corner.find(key(x, street_y[2] + corner_offset));
            if (c != corner.end()) add_edge(d, c->second, top);
        }
    }
    std::sort(prom.begin(), prom.end());
    for (std::size_t i = 0; i + 1 < prom.size(); ++i) add_edge(d, prom[i].second, prom[i + 1].second);
    // Links from the harbor-front buildings' door nodes to the promenade are through the sidewalk lines; the avenue corners above connect the two.
    for (const Pier& p : d.piers) {
        const double x = p.deck.cx();
        const std::uint32_t base = add_node(d, x, py);
        std::uint32_t nearest = prom.front().second;
        double best = 1e18;
        for (const auto& [t, n] : prom) {
            if (std::abs(t - x) < best) {
                best = std::abs(t - x);
                nearest = n;
            }
        }
        add_edge(d, nearest, base);
        std::uint32_t prev = base;
        for (double y = p.deck.y0 + 2.0; y < p.deck.y1 - 1.0; y += 8.0) {
            const std::uint32_t n = add_node(d, x, y);
            add_edge(d, prev, n);
            prev = n;
        }
        const std::uint32_t tip = add_node(d, x, p.deck.y1 - 1.0);
        add_edge(d, prev, tip);
    }
    // The park: a path from the sidewalk through the park and back.
    for (const Rect& p : d.park) {
        const double sy = p.y1 + 1.75;  // the sidewalk line along the street to the north
        const auto a = find_line(true, sy);
        if (a == nullptr) continue;
        std::uint32_t prev = 0;
        bool first = true;
        const double cx = p.cx();
        const auto pts = park_path_points(p);
        for (const auto& q : pts) {
            const std::uint32_t n = add_node(d, q[0], q[1]);
            if (!first) add_edge(d, prev, n);
            else {
                // attach to the sidewalk line
                a->nodes.push_back({q[0], n});
            }
            if (first == false && q[1] == sy) a->nodes.push_back({q[0], n});
            prev = n;
            first = false;
        }
        std::sort(a->nodes.begin(), a->nodes.end());
        // Re-chain this line's park attachments: connect each park end into the line by an edge to its neighbours.
        for (std::size_t i = 0; i < a->nodes.size(); ++i) {
            if (std::abs(a->nodes[i].first - (cx - 24.0)) < 1e-6 || std::abs(a->nodes[i].first - (cx + 24.0)) < 1e-6) {
                if (i > 0) add_edge(d, a->nodes[i - 1].second, a->nodes[i].second);
                if (i + 1 < a->nodes.size()) add_edge(d, a->nodes[i].second, a->nodes[i + 1].second);
            }
        }
    }
    // Interior doors: an outside node linked to the door's sidewalk node and an inside node linked to it.
    for (const Interior& in : d.interiors) {
        const auto it = door_node.find(in.building);
        if (it == door_node.end()) continue;
        const std::uint32_t outside = add_node(d, in.door_outside[0], in.door_outside[1]);
        const std::uint32_t inside = add_node(d, in.door_inside[0], in.door_inside[1]);
        add_edge(d, it->second, outside);
        add_edge(d, outside, inside);
    }
    // Non-interior buildings: the NPC walks to the door node and enters (the venue's anchor).
    // Mark crossing nodes.
    for (const PedEdge& e : d.ped_edges) {
        if (e.crosswalk == 0) continue;
        d.ped_nodes[e.a].crosswalk = e.crosswalk;
        d.ped_nodes[e.b].crosswalk = e.crosswalk;
    }

    // ---- interior navigation: cells on a 0.5 m grid, one slice per walkable cell ------------------------------------------------------------------------------------------------------------------------
    const double cell = 0.5;
    auto cell_of = [&](double v) { return static_cast<std::int32_t>(std::floor(v / cell)); };
    for (const Interior& in : d.interiors) {
        std::vector<Aabb> blockers;
        for (const StreetProp& p : in.furniture) {
            Aabb b;
            if (prop_box(p, b)) blockers.push_back(b);
        }
        std::vector<Aabb> upper_blockers;
        for (const StreetProp& p : in.upper_furniture) {
            Aabb b;
            if (prop_box(p, b)) upper_blockers.push_back(b);
        }
        auto blocked = [&](const std::vector<Aabb>& bs, double x, double y) {
            for (const Aabb& b : bs) {
                if (x > b.min.x - 0.2 && x < b.max.x + 0.2 && y > b.min.y - 0.2 && y < b.max.y + 0.2) return true;
            }
            return false;
        };
        const Rect& r = in.room;
        for (double y = std::ceil(r.y0 / cell) * cell + 0.25; y < r.y1 - 0.2; y += cell) {
            for (double x = std::ceil(r.x0 / cell) * cell + 0.25; x < r.x1 - 0.2; x += cell) {
                if (in.has_stairs && in.stair_footprint.contains(x, y)) continue;
                multi_floor_nav::FloorSlice s;
                s.id = {cell_of(x), cell_of(y), 0};
                s.z_m = 0.15;
                s.is_interior = true;
                s.walkable = !blocked(blockers, x, y);
                d.interior_nav.set_slice(s);
                if (in.has_stairs && r.contains(x, y)) {
                    // The upper floor, except over the stair well.
                    multi_floor_nav::FloorSlice u;
                    u.id = {cell_of(x), cell_of(y), 1};
                    u.z_m = in.upper_floor_z;
                    u.is_interior = true;
                    const bool in_well = in.stair_footprint.contains(x, y);
                    u.walkable = !in_well && !blocked(upper_blockers, x, y);
                    d.interior_nav.set_slice(u);
                }
            }
        }
        if (in.has_stairs) {
            multi_floor_nav::NavPortalLink p;
            p.id = in.building;
            // The portal starts on the landing in front of the first step (the steps' own cells are not floor cells) and ends just past the last.
            const double sx = in.stair_top[0] - in.stair_bottom[0], sy = in.stair_top[1] - in.stair_bottom[1];
            const double sl = std::hypot(sx, sy);
            const double lx = in.stair_bottom[0] - 0.7 * sx / sl, ly = in.stair_bottom[1] - 0.7 * sy / sl;
            p.from = {cell_of(lx), cell_of(ly), 0};
            p.to = {cell_of(in.stair_top[0]), cell_of(in.stair_top[1]), 1};
            p.kind = multi_floor_nav::PortalKind::Stair;
            p.traversal_cost = 6.0;
            p.traversal_duration_sec = 4.5;
            d.interior_nav.add_portal(p);
        }
    }
}

void build_venues(District& d, Rng& rng) {
    (void)rng;
    world_sim::VenueId next = 1;
    for (Building& b : d.buildings) {
        world_sim::Venue v;
        v.id = next++;
        const auto dir = leg_dir(b.front);
        const double ax = b.door_x - dir[0] * 1.2, ay = b.door_y - dir[1] * 1.2;
        v.anchor_position = world_sim::WorldPosition::from_world_double({ax, ay, 0.15});
        switch (b.kind) {
        case BuildingKind::Cafe: v.category = world_sim::VenueCategory::Hospitality; v.open_minute = 420; v.close_minute = 1320; v.capacity = 26; v.staff_required = 2; break;
        case BuildingKind::Bar: v.category = world_sim::VenueCategory::Hospitality; v.open_minute = 960; v.close_minute = 1439; v.capacity = 24; v.staff_required = 1; break;
        case BuildingKind::Bakery: v.category = world_sim::VenueCategory::Commercial; v.open_minute = 390; v.close_minute = 1020; v.capacity = 14; v.staff_required = 1; break;
        case BuildingKind::Shop: v.category = world_sim::VenueCategory::Commercial; v.open_minute = 480; v.close_minute = 1260; v.capacity = 18; v.staff_required = 1; break;
        case BuildingKind::Pharmacy: v.category = world_sim::VenueCategory::Commercial; v.open_minute = 540; v.close_minute = 1140; v.capacity = 12; v.staff_required = 1; break;
        case BuildingKind::Hotel: v.category = world_sim::VenueCategory::Hospitality; v.open_minute = 0; v.close_minute = 1439; v.capacity = 30; v.staff_required = 1; break;
        case BuildingKind::Offices: v.category = world_sim::VenueCategory::Commercial; v.open_minute = 480; v.close_minute = 1080; v.capacity = 20; v.staff_required = 0; break;
        case BuildingKind::Apartments: v.category = world_sim::VenueCategory::Residential; v.open_minute = 0; v.close_minute = 1439; v.capacity = 40; v.staff_required = 0; break;
        case BuildingKind::FireStation: v.category = world_sim::VenueCategory::EmergencyStation; v.open_minute = 0; v.close_minute = 1439; v.capacity = 10; v.staff_required = 0; break;
        default: v.category = world_sim::VenueCategory::Industrial; v.open_minute = 0; v.close_minute = 1439; v.capacity = 10; break;
        }
        b.venue = v.id;
        d.venues.push_back(v);
    }
    // Civic venues: the park, the promenade bench and the bus stop.
    for (const Rect& p : d.park) {
        world_sim::Venue v;
        v.id = next++;
        v.category = world_sim::VenueCategory::Civic;
        v.anchor_position = world_sim::WorldPosition::from_world_double({p.cx(), p.cy() - 6.0, 0.0});
        v.open_minute = 0;
        v.close_minute = 1439;
        v.capacity = 60;
        d.venues.push_back(v);
    }
    {
        world_sim::Venue v;
        v.id = next++;
        v.category = world_sim::VenueCategory::Civic;
        v.anchor_position = world_sim::WorldPosition::from_world_double({0.0, 0.5 * (d.promenade.y0 + d.promenade.y1), 0.15});
        v.open_minute = 0;
        v.close_minute = 1439;
        v.capacity = 60;
        d.venues.push_back(v);
    }
}

void build_collision(District& d) {
    std::vector<Aabb>& c = d.collision;
    // Ground: one slab under everything, the sidewalks raised, a sea bed.
    c.push_back(box(d.bounds.x0 - 100.0, d.bounds.y0 - 100.0, -3.0, d.bounds.x1 + 100.0, d.quay_y, 0.0));   // the road stubs run 80 m past the district edge
    for (const Rect& r : d.sidewalk) c.push_back(box(r.x0, r.y0, 0.0, r.x1, std::min(r.y1, d.quay_y), 0.15));
    c.push_back(box(d.bounds.x0 - 25.0, d.quay_y, -8.0, d.bounds.x1 + 25.0, d.quay_y + 140.0, -6.0));
    c.push_back(box(d.bounds.x0 - 25.0, d.quay_y - 0.6, -6.0, d.bounds.x1 + 25.0, d.quay_y, 0.15));  // the bulkhead below the promenade edge
    for (const Pier& p : d.piers) c.push_back(box(p.deck.x0, p.deck.y0, -0.1, p.deck.x1, p.deck.y1, p.z));
    // Buildings.
    for (const Building& b : d.buildings) {
        const Rect& f = b.footprint;
        if (!b.interior) {
            c.push_back(box(f.x0, f.y0, 0.0, f.x1, f.y1, b.height()));
            continue;
        }
        const Interior* in = nullptr;
        for (const Interior& i : d.interiors) {
            if (i.building == b.id) in = &i;
        }
        if (in == nullptr) continue;
        for (const Rect& w : in->walls) c.push_back(box(w.x0, w.y0, 0.0, w.x1, w.y1, b.height()));
        c.push_back(box(f.x0, f.y0, 0.0, f.x1, f.y1, 0.15));  // the floor
        if (!in->has_stairs) {
            c.push_back(box(f.x0, f.y0, b.floor_height - 0.25, f.x1, f.y1, b.height()));  // a ceiling and the mass above
        } else {
            // First-floor slab with the stair well open, the steps, and the upper room's roof.
            const double uz = in->upper_floor_z;
            const Rect& w = in->stair_footprint;
            const Rect& r = in->room;
            c.push_back(box(r.x0, r.y0, uz - 0.25, w.x0, r.y1, uz));
            c.push_back(box(w.x1, r.y0, uz - 0.25, r.x1, r.y1, uz));
            c.push_back(box(w.x0, r.y0, uz - 0.25, w.x1, w.y0, uz));
            c.push_back(box(w.x0, w.y1, uz - 0.25, w.x1, r.y1, uz));
            // Steps along the stair's run (which runs along the facade-normal axis).
            const bool along_y = (in->stair_footprint.h() > in->stair_footprint.w());
            const double rise = (uz - 0.15) / 12.0;
            const auto& bot = in->stair_bottom;
            const auto& top = in->stair_top;
            const double dir_x = along_y ? 0.0 : (top[0] > bot[0] ? 1.0 : -1.0);
            const double dir_y = along_y ? (top[1] > bot[1] ? 1.0 : -1.0) : 0.0;
            const double run = 0.3;
            for (int i = 0; i < 12; ++i) {
                const double s0 = bot[along_y ? 1 : 0] * 0 + i * run;
                const double sa = s0 - 0.15, sb = s0 + run - 0.15;  // the first step starts just before the bottom point
                const double x0 = along_y ? w.x0 : (dir_x > 0 ? bot[0] + sa : bot[0] - sb);
                const double x1 = along_y ? w.x1 : (dir_x > 0 ? bot[0] + sb : bot[0] - sa);
                const double y0 = along_y ? (dir_y > 0 ? bot[1] + sa : bot[1] - sb) : w.y0;
                const double y1 = along_y ? (dir_y > 0 ? bot[1] + sb : bot[1] - sa) : w.y1;
                c.push_back(box(x0, y0, 0.15, x1, y1, 0.15 + (i + 1) * rise));
            }
            c.push_back(box(f.x0, f.y0, b.height() - 0.25, f.x1, f.y1, b.height()));
            for (const StreetProp& p : in->upper_furniture) {
                Aabb pb;
                if (prop_box(p, pb)) c.push_back(pb);
            }
        }
        for (const StreetProp& p : in->furniture) {
            Aabb pb;
            if (prop_box(p, pb)) c.push_back(pb);
        }
    }
    for (const StreetProp& p : d.props) {
        Aabb pb;
        if (prop_box(p, pb)) c.push_back(pb);
    }
}

// ---- queries -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

std::uint32_t nearest_ped_node(const District& d, double x, double y) {
    std::uint32_t best = 0;
    double bd = std::numeric_limits<double>::infinity();
    for (std::uint32_t i = 0; i < d.ped_nodes.size(); ++i) {
        if (d.ped_nodes[i].layer != 0) continue;
        const double dd = std::hypot(d.ped_nodes[i].x - x, d.ped_nodes[i].y - y);
        if (dd < bd) {
            bd = dd;
            best = i;
        }
    }
    return best;
}

std::vector<std::uint32_t> ped_path(const District& d, std::uint32_t from, std::uint32_t to, bool walk_crossings) {
    const std::size_t n = d.ped_nodes.size();
    if (from >= n || to >= n) return {};
    // Adjacency by scanning the edge list into a temporary table (the network is small).
    std::vector<std::vector<std::pair<std::uint32_t, double>>> adj(n);
    for (const PedEdge& e : d.ped_edges) {
        if (e.crosswalk != 0 && !walk_crossings) continue;
        const double w = std::hypot(d.ped_nodes[e.a].x - d.ped_nodes[e.b].x, d.ped_nodes[e.a].y - d.ped_nodes[e.b].y) + (e.crosswalk != 0 ? 4.0 : 0.0);
        adj[e.a].push_back({e.b, w});
        adj[e.b].push_back({e.a, w});
    }
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<std::int64_t> prev(n, -1);
    using Item = std::pair<double, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    dist[from] = 0.0;
    q.push({0.0, from});
    while (!q.empty()) {
        const auto [dd, u] = q.top();
        q.pop();
        if (dd > dist[u]) continue;
        if (u == to) break;
        for (const auto& [v, w] : adj[u]) {
            if (dd + w < dist[v]) {
                dist[v] = dd + w;
                prev[v] = u;
                q.push({dist[v], v});
            }
        }
    }
    if (!std::isfinite(dist[to])) return {};
    std::vector<std::uint32_t> path;
    for (std::int64_t v = to; v >= 0; v = prev[static_cast<std::size_t>(v)]) path.push_back(static_cast<std::uint32_t>(v));
    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace saige::alive
