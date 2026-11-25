#include "world.hpp"

#include "district_detail.hpp"
#include "saige/capsule_character.hpp"
#include "saige/crowd_avoidance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <span>

namespace saige::alive {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStep = 1.0 / 60.0;
constexpr double kWalkSpeed = 1.40;
constexpr double kRunSpeed = 3.1;
constexpr double kCrossingNeed = 10.5;   // seconds a crossing takes at walking speed, plus margin
constexpr double kClearance = 4.0;       // the walk phase's flashing and all-red seconds that still protect a pedestrian already on the road

using world_sim::NpcIntent;
using world_sim::VenueId;

struct Route {
    std::vector<std::array<double, 3>> points;   // x, y, z feet
    std::vector<std::uint32_t> edge_crosswalk;   // per point: the crosswalk to be crossed on arriving at that point (0 none)
    std::vector<std::uint8_t> floor;             // per point
    std::size_t cursor = 0;
    [[nodiscard]] bool done() const { return cursor >= points.size(); }
};

struct Ped {
    std::uint64_t id = 0;
    std::unique_ptr<CapsuleCharacter> body;
    PedState state = PedState::Dwelling;
    Route route;
    VenueId goal_venue = world_sim::kInvalidVenueId;
    NpcIntent goal_intent = NpcIntent::Idle;
    long goal_signature = -1;
    bool visible = true;
    bool in_interior = false;
    int floor = 0;
    double dwell_until = 0.0;
    std::uint32_t crossing = 0;     // the crosswalk held now
    double yaw = 0.0;
    double speed = 0.0;
    double max_speed = kWalkSpeed;
    double last_move_time = 0.0;
    double last_x = 0, last_y = 0;
    int palette = 0;
    std::uint32_t home_building = 0;
    bool arrived_counted = false;
    double geom_cx = 0, geom_cy = 0;   // the centre of the part of the district this body collides against
};

} // namespace

struct PerfClock {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    double lap() {
        const auto n = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(n - t).count();
        t = n;
        return ms;
    }
};

struct World::Impl {
    WorldConfig cfg;
    District d;
    world_sim::WorldSimulation sim;
    crowd::CrowdSolver crowd;
    std::vector<Ped> peds;
    Rng rng;
    double accumulator = 0.0;
    double time = 0.0;
    double next_think = 0.0;
    int crowd_phase = 0;
    std::uint32_t next_event_id = 5000;
    double focus_x = 0.0, focus_y = 0.0;
    WorldStats st;
    std::map<std::uint32_t, int> crosswalk_load;   // pedestrians on each crosswalk

    explicit Impl(const WorldConfig& c) : cfg(c), d(build_district(c.seed)), sim(world_sim::SimBudgetConfig{}), crowd(crowd::CrowdSolverConfig{}), rng(c.seed * 7919u + 13u) {
        sim.clock().time_scale = cfg.time_scale;
        sim.clock().minute_of_day = cfg.start_minute;
        for (const world_sim::Venue& v : d.venues) sim.register_venue(v);
        populate();
        std::vector<crowd::ObstacleSegment> walls;
        crowd::ObstacleId id = 1;
        for (const Building& b : d.buildings) {
            const Rect& f = b.footprint;
            if (b.interior) continue;  // an accessible building's walls come from its interior below
            // The street-facing facade only: the sides that face other buildings are never walked along (the bodies' own collision covers everything else).
            switch (b.front) {
            case Leg::South: walls.push_back({id++, {f.x0, f.y0}, {f.x1, f.y0}}); break;
            case Leg::North: walls.push_back({id++, {f.x1, f.y1}, {f.x0, f.y1}}); break;
            case Leg::East: walls.push_back({id++, {f.x1, f.y0}, {f.x1, f.y1}}); break;
            case Leg::West: walls.push_back({id++, {f.x0, f.y1}, {f.x0, f.y0}}); break;
            }
        }
        for (const Interior& in : d.interiors) {
            for (const Rect& w : in.walls) {
                walls.push_back({id++, {w.x0, w.y0}, {w.x1, w.y0}});
                walls.push_back({id++, {w.x1, w.y0}, {w.x1, w.y1}});
                walls.push_back({id++, {w.x1, w.y1}, {w.x0, w.y1}});
                walls.push_back({id++, {w.x0, w.y1}, {w.x0, w.y0}});
            }
        }
        crowd.set_wall_segments(std::move(walls));
    }

    // ---- population -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    [[nodiscard]] const Building* building_of_venue(VenueId v) const {
        for (const Building& b : d.buildings)
            if (b.venue == v) return &b;
        return nullptr;
    }
    [[nodiscard]] const Interior* interior_of(std::uint32_t building) const {
        for (const Interior& i : d.interiors)
            if (i.building == building) return &i;
        return nullptr;
    }

    // A standing place near (x, y) that is clear of every solid, found on widening rings.
    [[nodiscard]] Vec3 free_spot(double x, double y) {
        for (int ring = 0; ring < 12; ++ring) {
            for (int k = 0; k < 12; ++k) {
                const double a = rng.range(0.0, 2.0 * kPi), r = ring == 0 ? 0.0 : 0.5 * ring;
                const double px = x + std::cos(a) * r, py = y + std::sin(a) * r;
                const Capsule cap{Vec3{static_cast<float>(px), static_cast<float>(py), static_cast<float>(ground_height(d, px, py) + 0.02)}, 0.34f, 1.75f};
                bool clear = true;
                for (const Ped& o : peds)   // and clear of everybody already standing about
                    if (o.visible && std::hypot(o.body->state().feet.x - px, o.body->state().feet.y - py) < 0.75) { clear = false; break; }
                if (!clear) continue;
                for (const Aabb& b : d.collision)
                    if (capsule_box_gap(cap, b) < 0.02f && !(b.max.z <= cap.feet.z + 0.001f)) { clear = false; break; }
                if (clear) return cap.feet;
            }
        }
        return Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(ground_height(d, x, y) + 0.02)};
    }

    // Each body collides against the boxes within 30 m of where it is (a body's sweeps scan its whole set), renewed when it has walked 10 m from the centre of that set.
    [[nodiscard]] std::vector<Aabb> local_boxes(double cx, double cy) const {
        std::vector<Aabb> out;
        for (const Aabb& b : d.collision) {
            const double dx = std::max({static_cast<double>(b.min.x) - cx, 0.0, cx - static_cast<double>(b.max.x)});
            const double dy = std::max({static_cast<double>(b.min.y) - cy, 0.0, cy - static_cast<double>(b.max.y)});
            if (dx * dx + dy * dy <= 30.0 * 30.0) out.push_back(b);
        }
        return out;
    }
    void refresh_geometry(Ped& p) const {
        const Vec3 f = p.body->state().feet;
        if (std::hypot(f.x - p.geom_cx, f.y - p.geom_cy) < 10.0) return;
        const auto boxes = local_boxes(f.x, f.y);
        if (p.body->replace_static_geometry(boxes)) {
            p.geom_cx = f.x;
            p.geom_cy = f.y;
        }
    }

    void populate() {
        std::vector<VenueId> homes, shops, eateries, bars, offices, civic;
        for (const Building& b : d.buildings) {
            switch (b.kind) {
            case BuildingKind::Apartments: case BuildingKind::Hotel: homes.push_back(b.venue); break;
            case BuildingKind::Shop: case BuildingKind::Pharmacy: shops.push_back(b.venue); break;
            case BuildingKind::Cafe: case BuildingKind::Bakery: eateries.push_back(b.venue); break;
            case BuildingKind::Bar: bars.push_back(b.venue); break;
            case BuildingKind::Offices: offices.push_back(b.venue); break;
            default: break;
            }
        }
        for (const world_sim::Venue& v : d.venues)
            if (v.category == world_sim::VenueCategory::Civic) civic.push_back(v.id);
        auto pick = [&](const std::vector<VenueId>& v) { return v.empty() ? world_sim::kInvalidVenueId : v[static_cast<std::size_t>(rng.pick(static_cast<int>(v.size())))]; };

        std::uint64_t next_id = 1000;
        auto add = [&](world_sim::OccupationKind occ, VenueId work, VenueId home) {
            world_sim::NpcAgent n;
            n.id = next_id++;
            n.persistent_memory.home_venue = home;
            n.persistent_memory.work_venue = work;
            n.occupation = occ;
            n.personality.bravery = rng.range(0.2, 0.9);
            n.personality.sociability = rng.range(0.2, 0.9);
            n.personality.conscientiousness = rng.range(0.4, 0.95);
            n.personality.curiosity = rng.range(0.2, 0.9);
            n.drives.energy = rng.range(0.6, 0.95);
            n.drives.hunger = rng.range(0.2, 0.7);
            n.drives.social = rng.range(0.3, 0.7);
            // Schedule (game minutes of the day): the working day is the venue's own; everyone else has a morning of errands and an afternoon by the water, with a home block under it all.
            const double t0 = cfg.start_minute - rng.range(0.0, 20.0);
            auto blk = [&](double a, double b, VenueId v, NpcIntent i) {
                n.schedule.push_back({static_cast<std::uint16_t>(static_cast<int>(std::fmod(a + 1440.0, 1440.0))), static_cast<std::uint16_t>(static_cast<int>(std::fmod(b + 1440.0, 1440.0))), v, i});
            };
            if (occ == world_sim::OccupationKind::Shopkeeper || occ == world_sim::OccupationKind::OfficeWorker) {
                blk(t0 - 30.0, t0 + 600.0, work, NpcIntent::Work);
                blk(t0 + 600.0, t0 + 900.0, home, NpcIntent::Rest);
            } else {
                const bool morning_cafe = rng.unit() < 0.6;
                blk(t0, t0 + 50.0 + rng.range(0.0, 20.0), morning_cafe ? pick(eateries) : pick(shops), morning_cafe ? NpcIntent::Socialize : NpcIntent::Shop);
                blk(t0 + 70.0, t0 + 140.0, pick(shops), NpcIntent::Shop);
                blk(t0 + 150.0, t0 + 260.0, rng.unit() < 0.5 ? pick(civic) : pick(eateries), NpcIntent::Socialize);
                blk(t0 + 270.0, t0 + 380.0, rng.unit() < 0.4 ? pick(bars) : pick(civic), NpcIntent::Socialize);
                blk(t0 + 400.0, t0 + 600.0, home, NpcIntent::Rest);
            }
            blk(t0 + 600.0, t0 + 1470.0, home, NpcIntent::Rest);
            // Spawn: workers at their venue's door (about to start), the rest on their home street.
            const world_sim::Venue* sv = sim.find_venue(occ == world_sim::OccupationKind::Unemployed ? home : work);
            const auto p = sv ? sv->anchor_position.to_world_double() : world_sim::Vec3d{0, 0, 0};
            n.position = world_sim::WorldPosition::from_world_double({p.x, p.y, 0.15});
            sim.register_npc(n);
            Ped ped;
            ped.id = n.id;
            const std::uint32_t node = nearest_ped_node(d, p.x, p.y);
            Vec3 spawn = free_spot(d.ped_nodes[node].x, d.ped_nodes[node].y);
            CapsuleCharacterConfig cc;
            cc.radius = 0.28f;
            cc.height = 1.75f;
            const auto local = local_boxes(spawn.x, spawn.y);
            ped.body = std::make_unique<CapsuleCharacter>(spawn, std::span<const Aabb>(local), cc);
            ped.geom_cx = spawn.x;
            ped.geom_cy = spawn.y;
            ped.palette = rng.pick(8);
            ped.max_speed = kWalkSpeed * rng.range(0.85, 1.15);
            ped.last_x = spawn.x;
            ped.last_y = spawn.y;
            peds.push_back(std::move(ped));
            crowd::CrowdAgent a;
            a.id = n.id;
            a.position = {spawn.x, spawn.y};
            a.destination = a.position;
            a.body_radius = 0.28;
            a.arrival_radius = 0.3;
            a.max_speed = peds.back().max_speed;
            crowd.add_agent(a);
        };
        // Staff: each venue with staff_required gets people with the matching occupation; offices get a few clerks.
        for (const Building& b : d.buildings) {
            const world_sim::Venue* v = sim.find_venue(b.venue);
            if (v == nullptr) continue;
            int staff = static_cast<int>(v->staff_required);
            if (b.kind == BuildingKind::Offices) staff = 2;
            for (int i = 0; i < staff && static_cast<int>(peds.size()) < cfg.residents; ++i)
                add(b.kind == BuildingKind::Offices ? world_sim::OccupationKind::OfficeWorker : world_sim::OccupationKind::Shopkeeper, b.venue, pick(homes));
        }
        while (static_cast<int>(peds.size()) < cfg.residents) add(world_sim::OccupationKind::Unemployed, world_sim::kInvalidVenueId, pick(homes));
    }

    // ---- routes ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    [[nodiscard]] static std::int32_t cell_of(double v) { return static_cast<std::int32_t>(std::floor(v / 0.5)); }

    void append_node_route(Route& r, const std::vector<std::uint32_t>& path) {
        for (std::size_t i = 0; i < path.size(); ++i) {
            const PedNode& n = d.ped_nodes[path[i]];
            std::uint32_t cw = 0;
            if (i > 0) {
                for (const PedEdge& e : d.ped_edges)
                    if (e.crosswalk != 0 && ((e.a == path[i - 1] && e.b == path[i]) || (e.b == path[i - 1] && e.a == path[i]))) cw = e.crosswalk;
            }
            r.points.push_back({n.x, n.y, ground_height(d, n.x, n.y)});
            r.edge_crosswalk.push_back(cw);
            r.floor.push_back(0);
        }
    }

    // A walkable cell of the room for a visitor, or the back of the room for staff.
    bool interior_target(const Interior& in, bool staff, std::int32_t& gx, std::int32_t& gy, std::uint8_t& layer) {
        const Rect& r = in.room;
        for (int attempt = 0; attempt < 40; ++attempt) {
            double x, y;
            if (staff) {
                // The farthest third from the door.
                const double dx = r.cx() - in.door_inside[0], dy = r.cy() - in.door_inside[1];
                x = r.cx() + dx * rng.range(0.3, 0.8) + rng.range(-1.0, 1.0);
                y = r.cy() + dy * rng.range(0.3, 0.8) + rng.range(-1.0, 1.0);
            } else {
                x = rng.range(r.x0 + 0.6, r.x1 - 0.6);
                y = rng.range(r.y0 + 0.6, r.y1 - 0.6);
            }
            layer = (in.has_stairs && !staff && rng.unit() < 0.35) ? 1 : 0;
            gx = cell_of(x);
            gy = cell_of(y);
            const auto* s = d.interior_nav.find_slice({gx, gy, layer});
            if (s != nullptr && s->walkable) return true;
        }
        return false;
    }

    // Plans the walk to a venue: along the street network to its door, and into the building when it has an interior.
    bool plan_to_venue(Ped& p, const Building& b, bool staff) {
        const Vec3 f = p.body->state().feet;
        const std::uint32_t from = nearest_ped_node(d, f.x, f.y);
        const auto dir = leg_dir(b.front);
        const std::uint32_t to = nearest_ped_node(d, b.door_x + dir[0] * 1.75, b.door_y + dir[1] * 1.75);
        const auto path = ped_path(d, from, to, true);
        if (path.empty()) return false;
        p.route = Route{};
        p.route.points.push_back({f.x, f.y, f.z});   // the first waypoint is where they stand
        p.route.edge_crosswalk.push_back(0);
        p.route.floor.push_back(0);
        append_node_route(p.route, path);
        if (const Interior* in = interior_of(b.id)) {
            // Outside the door, inside the door, then the room.
            p.route.points.push_back({in->door_outside[0], in->door_outside[1], 0.15});
            p.route.edge_crosswalk.push_back(0);
            p.route.floor.push_back(0);
            p.route.points.push_back({in->door_inside[0], in->door_inside[1], 0.16});
            p.route.edge_crosswalk.push_back(0);
            p.route.floor.push_back(0);
            std::int32_t gx, gy;
            std::uint8_t layer;
            if (interior_target(*in, staff, gx, gy, layer)) {
                const auto res = d.interior_nav.find_path({cell_of(in->door_inside[0]), cell_of(in->door_inside[1]), 0}, {gx, gy, layer});
                if (res.found) {
                    for (const auto& wp : res.waypoints) {
                        const auto* s = d.interior_nav.find_slice(wp);
                        p.route.points.push_back({(wp.gx + 0.5) * 0.5, (wp.gy + 0.5) * 0.5, (s ? s->z_m : 0.15) + 0.01});
                        p.route.edge_crosswalk.push_back(0);
                        p.route.floor.push_back(wp.layer);
                    }
                }
            }
        }
        p.route.cursor = 1;
        return true;
    }

    bool plan_to_point(Ped& p, double x, double y) {
        const Vec3 f = p.body->state().feet;
        const std::uint32_t from = nearest_ped_node(d, f.x, f.y), to = nearest_ped_node(d, x, y);
        const auto path = ped_path(d, from, to, true);
        if (path.empty()) return false;
        p.route = Route{};
        p.route.points.push_back({f.x, f.y, f.z});
        p.route.edge_crosswalk.push_back(0);
        p.route.floor.push_back(0);
        append_node_route(p.route, path);
        p.route.cursor = 1;
        return true;
    }

    // ---- the signal -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    [[nodiscard]] bool walk_allowed(std::uint32_t crosswalk) const {
        if (crosswalk == 0 || crosswalk > d.crosswalks.size()) return true;
        const Crosswalk& cw = d.crosswalks[crosswalk - 1];
        const traffic::IntersectionController* c = d.traffic.find_intersection(cw.intersection);
        if (c == nullptr || c->phases.empty() || c->emergency_preempted) return false;
        const auto& phase = c->phases[c->active_phase_idx];
        if (!phase.green_lanes.empty() || c->current_color != traffic::SignalColor::Green) return false;
        return (phase.green_duration_sec - c->phase_timer_sec) + kClearance >= kCrossingNeed;
    }
    [[nodiscard]] bool walk_lamp(std::uint32_t crosswalk) const {
        if (crosswalk == 0 || crosswalk > d.crosswalks.size()) return true;
        const traffic::IntersectionController* c = d.traffic.find_intersection(d.crosswalks[crosswalk - 1].intersection);
        if (c == nullptr || c->phases.empty() || c->emergency_preempted) return false;
        return c->phases[c->active_phase_idx].green_lanes.empty() && c->current_color == traffic::SignalColor::Green;
    }

    // ---- decisions ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void think() {
        for (Ped& p : peds) {
            world_sim::NpcAgent* n = sim.find_npc(p.id);
            if (n == nullptr) continue;
            const Vec3 f = p.body->state().feet;
            const auto& nav = n->current_nav_intent;
            const long sig = static_cast<long>(nav.intent) * 1000003L + static_cast<long>(nav.target_venue) * 131L + (nav.urgent ? 7 : 0) + (nav.has_destination ? 3 : 0) +
                             (nav.target_venue == world_sim::kInvalidVenueId && nav.has_destination ? static_cast<long>(nav.target_position.cell.x * 31 + nav.target_position.cell.y + std::lround(nav.target_position.local.x / 8.0f) * 1009 + std::lround(nav.target_position.local.y / 8.0f)) : 0);
            if (sig == p.goal_signature) continue;
            p.goal_signature = sig;
            p.goal_intent = nav.intent;
            p.goal_venue = nav.target_venue;
            p.arrived_counted = false;
            if (nav.intent == NpcIntent::Flee || nav.intent == NpcIntent::Investigate || nav.intent == NpcIntent::Assist) {
                const auto t = nav.target_position.to_world_double();
                if (p.state == PedState::Indoors) release(p);
                if (plan_to_point(p, t.x, t.y)) p.state = nav.intent == NpcIntent::Flee ? PedState::Fleeing : PedState::Walking;
                p.max_speed = nav.urgent ? kRunSpeed : kWalkSpeed * 1.15;
                continue;
            }
            p.max_speed = std::min(p.max_speed, kWalkSpeed * 1.15);
            if (p.max_speed < 1.0) p.max_speed = kWalkSpeed;
            if (!nav.has_destination || nav.target_venue == world_sim::kInvalidVenueId) {
                if (p.state == PedState::Walking && !p.route.done()) continue;
                p.state = PedState::Dwelling;
                p.dwell_until = time + 8.0;
                continue;
            }
            const Building* b = building_of_venue(nav.target_venue);
            if (b == nullptr) {
                // A civic venue (park, promenade): walk to its anchor.
                const auto t = nav.target_position.to_world_double();
                if (p.state == PedState::Indoors) release(p);
                if (plan_to_point(p, t.x, t.y)) p.state = PedState::Walking;
                continue;
            }
            if (p.state == PedState::Indoors) release(p);
            if (plan_to_venue(p, *b, nav.intent == NpcIntent::Work)) {
                p.state = PedState::Walking;
                (void)f;
            }
        }
    }

    // A hidden resident steps out of the door of the building they were in.
    void release(Ped& p) {
        if (p.state != PedState::Indoors) return;
        world_sim::NpcAgent* n = sim.find_npc(p.id);
        const auto a = n ? n->position.to_world_double() : world_sim::Vec3d{0, 0, 0};
        const std::uint32_t node = nearest_ped_node(d, a.x, a.y);
        p.geom_cx = 1e9;   // force a refresh around the new place before the body is moved
        const auto boxes = local_boxes(d.ped_nodes[node].x, d.ped_nodes[node].y);
        (void)p.body->replace_static_geometry(boxes);
        p.geom_cx = d.ped_nodes[node].x;
        p.geom_cy = d.ped_nodes[node].y;
        p.visible = false;   // not counted as in the way while a place is found
        const Vec3 spot = free_spot(d.ped_nodes[node].x, d.ped_nodes[node].y);
        (void)p.body->teleport(spot);
        crowd::CrowdAgent ag;
        ag.id = p.id;
        ag.position = {spot.x, spot.y};
        ag.destination = ag.position;
        ag.body_radius = 0.28;
        ag.max_speed = p.max_speed;
        crowd.add_agent(ag);
        p.visible = true;
        p.state = PedState::Walking;
    }

    // ---- stepping ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void substep() {
        time += kStep;
        PerfClock pc;
        d.traffic.step(kStep);
        sim.step(kStep, world_sim::WorldPosition::from_world_double({focus_x, focus_y, 0.0}));
        st.ms_sim += pc.lap();
        if (time >= next_think) {
            think();
            next_think = time + 0.5;
        }
        std::map<std::uint32_t, int> load;
        for (Ped& p : peds) {
            if (p.state == PedState::Indoors) continue;
            update_one(p, load);
        }
        crosswalk_load = load;
        for (const Crosswalk& c : d.crosswalks) {
            const bool occupied = load.count(c.id) != 0 && load[c.id] > 0;
            for (std::uint32_t z : c.traffic_zones) d.traffic.set_crosswalk_occupied(z, occupied);
        }
        st.ms_decide += pc.lap();
        if (++crowd_phase >= 3) {   // the crowd solver plans at 20 Hz; the bodies move at 60 Hz on the last plan
            crowd_phase = 0;
            const auto m = crowd.step(3.0 * kStep);
            st.min_separation_m = m.min_separation_m;
        }
        st.ms_crowd += pc.lap();
        for (Ped& p : peds) {
            if (p.state == PedState::Indoors) continue;
            const crowd::CrowdAgent* a = crowd.find_agent(p.id);
            if (a == nullptr) continue;
            CapsuleInput in;
            in.drive.active = true;
            in.drive.x = static_cast<float>(a->velocity.x);
            in.drive.y = static_cast<float>(a->velocity.y);
            refresh_geometry(p);
            p.body->step(in, std::chrono::nanoseconds(16666667));
            const Vec3 f = p.body->state().feet;
            const double vx = p.body->state().velocity.x, vy = p.body->state().velocity.y;
            p.speed = std::hypot(vx, vy);
            if (p.speed > 0.15) {
                const double target = std::atan2(vy, vx);
                double diff = target - p.yaw;
                while (diff > kPi) diff -= 2.0 * kPi;
                while (diff < -kPi) diff += 2.0 * kPi;
                p.yaw += std::clamp(diff, -8.0 * kStep, 8.0 * kStep);
            }
            if (std::hypot(f.x - p.last_x, f.y - p.last_y) > 0.5) {
                p.last_x = f.x;
                p.last_y = f.y;
                p.last_move_time = time;
            }
            if (world_sim::NpcAgent* n = sim.find_npc(p.id)) n->position = world_sim::WorldPosition::from_world_double({f.x, f.y, f.z});
        }
        st.ms_bodies += pc.lap();
    }

    void update_one(Ped& p, std::map<std::uint32_t, int>& load) {
        crowd::CrowdAgent* a = crowd.find_agent(p.id);
        if (a == nullptr) return;
        const Vec3 f = p.body->state().feet;
        a->position = {f.x, f.y};
        a->velocity = {p.body->state().velocity.x, p.body->state().velocity.y};
        a->max_speed = p.max_speed;
        p.floor = f.z > 1.0f ? 1 : 0;
        p.in_interior = false;
        for (const Interior& in : d.interiors)
            if (in.room.contains(f.x, f.y) && f.z < in.upper_floor_z + 3.0) p.in_interior = true;
        auto hold = [&] { a->has_destination = false; };   // stand still (the solver still keeps them clear of others)
        auto aim = [&](double x, double y) {
            if (!a->has_destination || a->arrived || std::abs(a->destination.x - x) > 1e-6 || std::abs(a->destination.y - y) > 1e-6) crowd.set_agent_destination(p.id, {x, y});   // arrival is sticky in the solver until a destination is set
        };
        switch (p.state) {
        case PedState::Dwelling:
            hold();
            if (time >= p.dwell_until) {
                if (p.goal_venue == world_sim::kInvalidVenueId || p.goal_intent == NpcIntent::Idle) {
                    // Wander a short way along the street.
                    const std::uint32_t here = nearest_ped_node(d, f.x, f.y);
                    const std::uint32_t there = static_cast<std::uint32_t>(rng.pick(static_cast<int>(d.ped_nodes.size())));
                    if (d.ped_nodes[there].layer == 0 && std::hypot(d.ped_nodes[there].x - d.ped_nodes[here].x, d.ped_nodes[there].y - d.ped_nodes[here].y) < 60.0 && plan_to_point(p, d.ped_nodes[there].x, d.ped_nodes[there].y)) p.state = PedState::Walking;
                    else p.dwell_until = time + 5.0;
                } else {
                    p.dwell_until = time + 10.0;
                }
            }
            break;
        case PedState::WaitingToCross: {
            hold();
            const std::uint32_t cw = p.route.edge_crosswalk[p.route.cursor];
            if (walk_allowed(cw)) {
                p.state = PedState::Crossing;
                p.crossing = cw;
                ++st.crossings_started;
                if (!walk_lamp(cw)) ++st.crossings_against_signal;
            }
            break;
        }
        case PedState::Walking:
        case PedState::Crossing:
        case PedState::Fleeing: {
            if (p.route.done()) {
                arrive(p, *a);
                break;
            }
            const auto& t = p.route.points[p.route.cursor];
            const std::uint32_t cw = p.route.edge_crosswalk[p.route.cursor];
            if (cw != 0 && p.state != PedState::Crossing && p.crossing != cw) {
                // At the kerb: stop where the sidewalk ends and wait for the signal.
                const auto& prev = p.route.points[p.route.cursor - 1];
                if (std::hypot(f.x - prev[0], f.y - prev[1]) < 1.4) {
                    if (p.state == PedState::Fleeing || walk_allowed(cw)) {
                        p.state = p.state == PedState::Fleeing ? PedState::Fleeing : PedState::Crossing;
                        p.crossing = cw;
                        ++st.crossings_started;
                        if (p.state != PedState::Fleeing && !walk_lamp(cw)) ++st.crossings_against_signal;
                    } else {
                        p.state = PedState::WaitingToCross;
                        hold();
                        break;
                    }
                }
            }
            aim(t[0], t[1]);
            if (std::hypot(f.x - t[0], f.y - t[1]) < (p.route.cursor + 1 < p.route.points.size() ? 1.0 : 0.8)) {   // generous: a crowd at a doorway cannot all stand on one point
                if (cw != 0) p.crossing = 0;
                ++p.route.cursor;
                if (p.state == PedState::Crossing) p.state = PedState::Walking;   // the next crosswalk, if any, is judged afresh at its kerb
            }
            if (p.state == PedState::Crossing && p.crossing != 0) ++load[p.crossing];
            break;
        }
        case PedState::Indoors: break;
        }
    }

    void arrive(Ped& p, crowd::CrowdAgent& a) {
        const Vec3 f = p.body->state().feet;
        a.has_destination = false;
        const Building* b = p.goal_venue == world_sim::kInvalidVenueId ? nullptr : building_of_venue(p.goal_venue);
        if (!p.arrived_counted && p.goal_venue != world_sim::kInvalidVenueId) {
            p.arrived_counted = true;
            ++st.venue_arrivals;
            if (b != nullptr && interior_of(b->id) != nullptr) {
                ++st.interior_arrivals;
                if (f.z > 1.0f) ++st.stair_climbs;
            }
        }
        if (b != nullptr && interior_of(b->id) == nullptr && p.goal_intent != NpcIntent::Idle && b->kind != BuildingKind::FireStation) {
            // No modelled interior: the door takes them in.
            p.state = PedState::Indoors;
            p.visible = false;
            crowd.remove_agent(p.id);
            if (world_sim::NpcAgent* n = sim.find_npc(p.id)) {
                const world_sim::Venue* v = sim.find_venue(b->venue);
                if (v != nullptr) n->position = v->anchor_position;
            }
            return;
        }
        p.state = PedState::Dwelling;
        p.dwell_until = time + rng.range(20.0, 60.0);
    }

    void collect_stats() {
        PerfClock pc;
        WorldStats s = st;
        s.pedestrians = static_cast<std::uint32_t>(peds.size());
        s.walking = s.waiting = s.crossing = s.dwelling = s.indoors = s.fleeing = 0;
        s.overlapping_with_geometry = 0;
        s.stuck = 0;
        for (const Ped& p : peds) {
            switch (p.state) {
            case PedState::Walking: ++s.walking; break;
            case PedState::WaitingToCross: ++s.waiting; break;
            case PedState::Crossing: ++s.crossing; break;
            case PedState::Dwelling: ++s.dwelling; break;
            case PedState::Indoors: ++s.indoors; break;
            case PedState::Fleeing: ++s.fleeing; break;
            }
            if (p.state != PedState::Indoors) {
                const Capsule cap = p.body->capsule();
                bool hit = false;
                for (const Aabb& b : d.collision)
                    if (capsule_box_gap(cap, b) < -0.03f) {
                        hit = true;
                        break;
                    }
                if (hit) ++s.overlapping_with_geometry;
                if (p.state == PedState::Walking && time - p.last_move_time > 100.0) ++s.stuck;
            }
        }
        s.game_minute = sim.clock().minute_of_day;
        s.day = sim.clock().day_index;
        st.ms_stats += pc.lap();
        s.ms_stats = st.ms_stats;
        cached = s;
    }
    WorldStats cached;
};

World::World(WorldConfig config) : impl_(std::make_unique<Impl>(config)) {}
World::~World() = default;

void World::step(double dt) {
    Impl& i = *impl_;
    i.accumulator += dt;
    while (i.accumulator >= kStep) {
        i.substep();
        i.accumulator -= kStep;
    }
    i.collect_stats();
}
void World::set_focus(double x, double y) {
    impl_->focus_x = x;
    impl_->focus_y = y;
}
const District& World::district() const { return impl_->d; }
double World::time_seconds() const { return impl_->time; }
double World::minute_of_day() const { return impl_->sim.clock().minute_of_day; }
WorldStats World::stats() const { return impl_->cached; }
bool World::walk_signal(std::uint32_t crosswalk) const { return impl_->walk_lamp(crosswalk); }

std::vector<PedView> World::pedestrians() const {
    std::vector<PedView> out;
    for (const Ped& p : impl_->peds) {
        PedView v;
        const Vec3 f = p.body->state().feet;
        v.id = p.id;
        v.x = f.x;
        v.y = f.y;
        v.z = f.z;
        v.yaw = p.yaw;
        v.speed = p.speed;
        v.state = p.state;
        v.visible = p.visible;
        v.inside_building = p.in_interior;
        v.floor = p.floor;
        v.palette = p.palette;
        v.stalled_s = impl_->time - p.last_move_time;
        if (!p.route.done()) {
            v.target_x = p.route.points[p.route.cursor][0];
            v.target_y = p.route.points[p.route.cursor][1];
        }
        if (const world_sim::NpcAgent* n = impl_->sim.find_npc(p.id)) {
            v.occupation = n->occupation;
            v.intent = n->current_nav_intent.intent;
        }
        out.push_back(v);
    }
    return out;
}

void World::report_danger(double x, double y, double radius_m, double duration_s) {
    (void)radius_m;
    impl_->sim.report_traffic_accident(++impl_->next_event_id, world_sim::WorldPosition::from_world_double({x, y, 0.0}), duration_s);
}

} // namespace saige::alive
