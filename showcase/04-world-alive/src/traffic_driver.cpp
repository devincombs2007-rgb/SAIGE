#include "traffic_driver.hpp"

#include "district_detail.hpp"
#include "saige/static_collision.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace saige::alive {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStep = 1.0 / 60.0;
constexpr double kRideHeight = 0.80;   // the chassis centre above the road when it is dropped onto its suspension

[[nodiscard]] VehicleConfig car_model_config() {
    // The geometry of the CarConcept model (Showcase 03 measured it): wheel mounts, radius and suspension are the model's, so the drawn wheels sit where the physics puts them.
    VehicleConfig c;
    c.mass = 1450.0f;
    c.half_extents = {2.17f, 1.0f, 0.53f};
    c.center_of_mass = {-0.14f, 0.0f, -0.30f};
    c.wheel_radius = 0.384f;
    c.suspension_travel = 0.40f;
    c.spring = 36000.0f;
    c.damper = 4300.0f;
    c.wheels = {{
        {{1.256f, 0.98f, 0.065f}, true, false},
        {{1.256f, -0.98f, 0.065f}, true, false},
        {{-1.544f, 0.98f, 0.065f}, false, true},
        {{-1.544f, -0.98f, 0.065f}, false, true},
    }};
    return c;
}

} // namespace

VehicleConfig vehicle_config_for(VehicleKind k) {
    VehicleConfig c = car_model_config();
    c.damage.enabled = true;
    c.slip_angle_lateral = true;
    switch (k) {
    case VehicleKind::Hatchback:
        c.mass = 1250.0f;
        c.engine_force = 7200.0f;
        break;
    case VehicleKind::Van:
        c.mass = 2350.0f;
        c.half_extents = {2.45f, 1.0f, 0.55f};
        c.center_of_mass = {-0.1f, 0.0f, -0.4f};
        c.wheel_radius = 0.40f;
        c.wheels = {{{{1.45f, 1.0f, 0.0f}, true, false}, {{1.45f, -1.0f, 0.0f}, true, false}, {{-1.5f, 1.0f, 0.0f}, false, true}, {{-1.5f, -1.0f, 0.0f}, false, true}}};
        c.engine_force = 8600.0f;
        c.engine_power = 130000.0f;
        c.brake_force = 20000.0f;
        break;
    case VehicleKind::Ambulance:
        c.mass = 3100.0f;
        c.half_extents = {2.8f, 1.0f, 0.6f};
        c.center_of_mass = {-0.1f, 0.0f, -0.5f};
        c.wheel_radius = 0.42f;
        c.wheels = {{{{1.7f, 1.0f, 0.0f}, true, false}, {{1.7f, -1.0f, 0.0f}, true, false}, {{-1.7f, 1.0f, 0.0f}, false, true}, {{-1.7f, -1.0f, 0.0f}, false, true}}};
        c.engine_force = 11000.0f;
        c.engine_power = 190000.0f;
        c.brake_force = 26000.0f;
        break;
    case VehicleKind::FireEngine:
        c.mass = 8200.0f;
        c.half_extents = {4.0f, 1.1f, 0.65f};
        c.center_of_mass = {-0.2f, 0.0f, -0.6f};
        c.wheel_radius = 0.5f;
        c.suspension_travel = 0.5f;
        c.spring = 70000.0f;
        c.damper = 9000.0f;
        c.wheels = {{{{2.7f, 1.1f, 0.0f}, true, false}, {{2.7f, -1.1f, 0.0f}, true, false}, {{-2.4f, 1.1f, 0.0f}, false, true}, {{-2.4f, -1.1f, 0.0f}, false, true}}};
        c.engine_force = 22000.0f;
        c.engine_power = 280000.0f;
        c.brake_force = 54000.0f;
        break;
    case VehicleKind::Taxi:
    case VehicleKind::Sedan: break;
    }
    return c;
}

namespace {

struct Car {
    std::uint32_t id = 0;
    VehicleKind kind = VehicleKind::Sedan;
    std::unique_ptr<Vehicle> body;
    std::vector<traffic::LaneId> route;
    std::size_t cursor = 0;
    double distract_until = -1.0;
    double distract_bias = 0.0;
    bool disabled = false;
    double disabled_at = 0.0;
    bool emergency = false;
    bool responder = false;   // dispatched by the response: never wrecked by the incident logic, whatever its siren state
    double on_scene_since = -1.0;
    bool braking = false;
    int palette = 0;
    float damage = 0.0f;
    bool remove = false;
    double spawned_at = 0.0;
    double stopped_since = -1.0;
};

[[nodiscard]] std::array<double, 2> lane_tangent(const traffic::LaneSegment& l) {
    const double dx = l.end.x - l.start.x, dy = l.end.y - l.start.y;
    const double n = std::hypot(dx, dy);
    return {dx / n, dy / n};
}

[[nodiscard]] double wrap(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

} // namespace

struct TrafficDriver::Impl {
    District& d;
    StaticCollision collision;
    QueryScratch scratch;
    Rng rng;
    int target = 0;
    bool accidents = true;
    double time = 0.0;
    std::uint32_t next_id = 1;
    std::vector<Car> cars;
    std::vector<TrafficEvent> events;
    TrafficStats st;
    double next_spawn = 0.5;
    double next_accident_roll = 25.0;
    std::uint32_t incident_car = 0;       // the wrecked car being dealt with (0: none)
    double incident_x = 0, incident_y = 0;
    traffic::LaneId incident_lane = 0;
    bool response_sent = false;
    std::vector<traffic::LaneId> exits;
    struct Curve {
        std::array<double, 2> p0, p1, p2;
        double length = 0.0;
        [[nodiscard]] std::array<double, 2> at(double t) const {
            const double a = (1 - t) * (1 - t), b = 2 * (1 - t) * t, c = t * t;
            return {a * p0[0] + b * p1[0] + c * p2[0], a * p0[1] + b * p1[1] + c * p2[1]};
        }
    };
    std::map<traffic::LaneId, Curve> curves;   // a connector through an intersection is driven along a smooth curve (a quadratic through the corner of its two roads), not along its chord

    Impl(District& dist, std::uint32_t seed, int target_vehicles, bool acc)
        : d(dist), collision(std::span<const Aabb>(dist.collision), std::span<const Triangle>{}), rng(seed * 104729u + 7u), target(target_vehicles), accidents(acc) {
        for (const Intersection& in : d.intersections)
            for (std::size_t leg = 0; leg < 4; ++leg) {
                if (!in.has_leg[leg]) continue;
                const traffic::LaneSegment* l = d.traffic.find_lane(in.outbound[leg]);
                if (l != nullptr && l->end_intersection == traffic::kInvalidIntersectionId && l->successor_lanes.empty()) exits.push_back(l->id);
            }
        for (const District::LanePath& lp : d.lane_paths) {
            if (!lp.connector) continue;
            const traffic::LaneSegment* c = d.traffic.find_lane(lp.id);
            if (c == nullptr || c->successor_lanes.empty()) continue;
            const traffic::LaneSegment* out = d.traffic.find_lane(c->successor_lanes.front());
            const traffic::LaneSegment* in = nullptr;
            for (const District::LanePath& other : d.lane_paths) {
                const traffic::LaneSegment* o = d.traffic.find_lane(other.id);
                if (o == nullptr || other.connector) continue;
                for (traffic::LaneId sl : o->successor_lanes)
                    if (sl == lp.id) in = o;
            }
            if (out == nullptr || in == nullptr) continue;
            Curve cv;
            cv.p0 = {c->start.x, c->start.y};
            cv.p2 = {c->end.x, c->end.y};
            const auto ti = lane_tangent(*in), to = lane_tangent(*out);
            // Intersection of the line p0 + a*ti with the line p2 - b*to.
            const double det = ti[0] * to[1] - ti[1] * to[0];
            if (std::fabs(det) < 1e-3) {
                cv.p1 = {0.5 * (cv.p0[0] + cv.p2[0]), 0.5 * (cv.p0[1] + cv.p2[1])};
            } else {
                const double dx = cv.p2[0] - cv.p0[0], dy = cv.p2[1] - cv.p0[1];
                const double a = (dx * to[1] - dy * to[0]) / det;
                cv.p1 = {cv.p0[0] + ti[0] * a, cv.p0[1] + ti[1] * a};
            }
            double len = 0.0;
            auto prev = cv.at(0.0);
            for (int i = 1; i <= 32; ++i) {
                const auto q = cv.at(i / 32.0);
                len += std::hypot(q[0] - prev[0], q[1] - prev[1]);
                prev = q;
            }
            cv.length = len;
            curves[lp.id] = cv;
        }
    }

    // ---- helpers -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    [[nodiscard]] Car* find(std::uint32_t id) {
        for (Car& c : cars)
            if (c.id == id) return &c;
        return nullptr;
    }
    [[nodiscard]] static double length_of(VehicleKind k) { return k == VehicleKind::FireEngine ? 8.0 : k == VehicleKind::Ambulance ? 5.6 : k == VehicleKind::Van ? 4.9 : k == VehicleKind::Hatchback ? 3.9 : 4.3; }

    [[nodiscard]] double lane_s(const traffic::LaneSegment& l, double x, double y) const {
        const auto t = lane_tangent(l);
        return (x - l.start.x) * t[0] + (y - l.start.y) * t[1];
    }
    [[nodiscard]] double lane_lateral(const traffic::LaneSegment& l, double x, double y) const {
        const auto t = lane_tangent(l);
        return -(x - l.start.x) * t[1] + (y - l.start.y) * t[0];
    }

    bool spawn(VehicleKind kind, const std::vector<traffic::LaneId>& route, double s0, bool emergency) {
        if (route.empty()) return false;
        const traffic::LaneSegment* lane = d.traffic.find_lane(route.front());
        if (lane == nullptr) return false;
        const auto t = lane_tangent(*lane);
        const double x = lane->start.x + t[0] * s0, y = lane->start.y + t[1] * s0;
        Car c;
        c.id = next_id++;
        c.kind = kind;
        c.route = route;
        c.emergency = emergency;
        c.responder = emergency;
        c.palette = rng.pick(7);
        c.spawned_at = time;
        c.body = std::make_unique<Vehicle>(vehicle_config_for(kind), Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(kRideHeight + 0.1)}, static_cast<float>(std::atan2(t[1], t[0])));
        traffic::TrafficVehicle tv;
        tv.id = c.id;
        tv.role = emergency ? traffic::VehicleRole::Emergency : (kind == VehicleKind::Van ? traffic::VehicleRole::Commercial : traffic::VehicleRole::Civilian);
        tv.current_lane = route.front();
        tv.s_m = s0;
        tv.speed_mps = 0.0;
        tv.length_m = length_of(kind);
        tv.desired_speed_mps = emergency ? 14.0 : rng.range(8.5, 11.0);
        tv.max_accel_mps2 = kind == VehicleKind::FireEngine ? 1.8 : 2.5;
        tv.comfort_decel_mps2 = 3.0;
        tv.min_gap_m = 2.5;
        tv.use_external_physics = true;
        tv.route_lanes = route;
        tv.route_cursor = 0;
        if (!d.traffic.add_vehicle(tv)) return false;
        cars.push_back(std::move(c));
        ++st.spawned;
        return true;
    }

    [[nodiscard]] bool lane_free_at_start(traffic::LaneId lane) const {
        for (const Car& c : cars) {
            const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
            if (tv != nullptr && tv->current_lane == lane && tv->s_m < 16.0) return false;
        }
        return true;
    }

    void maybe_spawn() {
        int civilians = 0;
        for (const Car& c : cars)
            if (!c.emergency) ++civilians;
        if (civilians >= target || time < next_spawn || d.entry_lanes.empty()) return;
        next_spawn = time + rng.range(1.5, 3.5);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const traffic::LaneId entry = d.entry_lanes[static_cast<std::size_t>(rng.pick(static_cast<int>(d.entry_lanes.size())))];
            if (!lane_free_at_start(entry)) continue;
            const traffic::LaneId exit = exits[static_cast<std::size_t>(rng.pick(static_cast<int>(exits.size())))];
            auto route = d.traffic.find_lane_route(entry, exit);
            if (route.size() < 4) continue;
            const int r = rng.pick(10);
            const VehicleKind k = r < 5 ? VehicleKind::Sedan : r < 7 ? VehicleKind::Hatchback : r < 9 ? VehicleKind::Van : VehicleKind::Taxi;
            if (spawn(k, route, 3.0, false)) return;
        }
    }

    // ---- the gap rule: distance to the nearest car ahead along this car's route ------------------------------------------------------------------------------------------------------
    [[nodiscard]] double gap_ahead(const Car& me, double my_s, double& lead_speed) const {
        double best = 1e9;
        lead_speed = 0.0;
        double offset = 0.0;   // from the car to the start of lane k along the route
        for (std::size_t k = 0; k < 3 && me.cursor + k < me.route.size(); ++k) {
            const traffic::LaneId lid = me.route[me.cursor + k];
            const traffic::LaneSegment* lane = d.traffic.find_lane(lid);
            if (lane == nullptr) break;
            for (const Car& o : cars) {
                if (&o == &me) continue;
                const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(o.id);
                if (tv == nullptr || tv->current_lane != lid) continue;
                if (k == 0 && tv->s_m <= my_s) continue;
                const double dist = k == 0 ? tv->s_m - my_s : offset + tv->s_m;
                const double gap = dist - 0.5 * (length_of(me.kind) + tv->length_m);
                if (gap < best) {
                    best = gap;
                    lead_speed = tv->speed_mps;
                }
            }
            offset += k == 0 ? lane->length_m() - my_s : lane->length_m();
        }
        return best;
    }

    // ---- stepping ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void substep() {
        time += kStep;
        auto t0 = std::chrono::steady_clock::now();
        // 1) read each chassis back into lane and distance (the host owns lane transitions)
        for (Car& c : cars) {
            const VehicleState& vs = c.body->state();
            const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
            if (tv == nullptr) continue;
            const traffic::LaneSegment* lane = d.traffic.find_lane(tv->current_lane);
            if (lane == nullptr) continue;
            auto s_on = [&](const traffic::LaneSegment& l) {
                const auto cu = curves.find(l.id);
                return cu != curves.end() ? curve_param(cu->second, vs.position.x, vs.position.y) * l.length_m() : lane_s(l, vs.position.x, vs.position.y);
            };
            double s = s_on(*lane);
            const double len = lane->length_m();
            if (s >= len - 0.4 && c.cursor + 1 < c.route.size()) {
                const traffic::LaneId prev = lane->id;
                ++c.cursor;
                const traffic::LaneSegment* next = d.traffic.find_lane(c.route[c.cursor]);
                if (next != nullptr) {
                    if (lane->end_intersection != traffic::kInvalidIntersectionId) {
                        const traffic::IntersectionController* ic = d.traffic.find_intersection(lane->end_intersection);
                        if (ic != nullptr && !c.emergency && ic->signal_for_lane(prev) == traffic::SignalColor::Red && !c.disabled) ++st.red_light_runs;
                    }
                    lane = next;
                    s = s_on(*lane);
                }
            }
            d.traffic.sync_external_vehicle_state(c.id, lane->id, std::clamp(s, 0.0, lane->length_m()), std::max(0.0, static_cast<double>(vs.forward_speed)));
            if (c.cursor + 1 >= c.route.size() && s >= lane->length_m() - 2.5) {
                if (!c.emergency) {
                    c.remove = true;
                } else if (!lane->successor_lanes.empty()) {
                    // A responder that has reached the end of its way to the scene without stopping (nothing ahead of it held it): it leaves by the nearest exit instead of running on.
                    const traffic::LaneId exit = exits[static_cast<std::size_t>(rng.pick(static_cast<int>(exits.size())))];
                    auto route = d.traffic.find_lane_route(lane->id, exit);
                    if (!route.empty()) {
                        c.route = route;
                        c.cursor = 0;
                        c.emergency = false;
                    }
                } else {
                    c.remove = true;   // the end of a road at the edge of the district
                }
            }
            if (!c.disabled && c.distract_until < time && !lane->successor_lanes.empty() && lane->end_intersection != traffic::kInvalidIntersectionId) {
                const double e = std::fabs(lane_lateral(*lane, vs.position.x, vs.position.y));
                st.max_lane_error_m = std::max(st.max_lane_error_m, e);
                if (e > 1.5 && s > 1.0 && s < lane->length_m() - 1.0) ++st.lane_departures;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        d.traffic.step(kStep);   // the signals advance and every car's wish is recomputed
        const auto t2 = std::chrono::steady_clock::now();
        // 2) control and physics
        for (Car& c : cars) {
            const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
            if (tv == nullptr) continue;
            const VehicleState& vs = c.body->state();
            const double speed = vs.forward_speed;
            VehicleInput in;
            if (c.disabled) {
                in.brake = 1.0f;
                in.handbrake = 1.0f;
            } else {
                in.throttle = static_cast<float>(tv->control_output.throttle);
                in.brake = static_cast<float>(tv->control_output.brake);
                const traffic::LaneSegment* lane = d.traffic.find_lane(tv->current_lane);
                double lead_speed = 0.0;
                const double gap = gap_ahead(c, tv->s_m, lead_speed);
                // Never drive into the car ahead faster than a firm stop would allow (there is no vehicle-vehicle contact to stop it).
                const double v_safe = std::sqrt(std::max(0.0, 2.0 * 4.5 * (gap - 2.0))) + lead_speed * 0.3;
                if (speed > v_safe + 0.3) {
                    in.throttle = 0.0f;
                    in.brake = std::max(in.brake, static_cast<float>(std::clamp((speed - v_safe) * 0.35, 0.0, 1.0)));
                }
                if (speed < 0.4 && in.throttle < 0.05f && in.brake > 0.0f) in.brake = 1.0f;   // hold still at a line
                // steering
                if (c.distract_until > time) {
                    in.steer = static_cast<float>(c.distract_bias);
                    in.throttle = std::max(in.throttle, 0.25f);
                    in.brake = 0.0f;
                } else if (lane != nullptr) {
                    in.steer = static_cast<float>(pursuit_steer(c, *tv, vs));
                }
                if (c.distract_until > 0.0 && c.distract_until <= time && c.distract_until + kStep > time) c.distract_until = -1.0;
            }
            c.braking = in.brake > 0.1f;
            c.body->step(in, static_cast<float>(kStep), collision, scratch);
            c.damage = *std::max_element(c.body->damage().zone.begin(), c.body->damage().zone.end());
            for (const VehicleImpact& im : c.body->take_impacts()) {
                TrafficEvent ev;
                ev.type = TrafficEvent::Type::Impact;
                ev.vehicle = c.id;
                ev.x = im.point.x;
                ev.y = im.point.y;
                ev.speed = im.speed;
                ev.energy = im.energy;
                ev.impulse = im.impulse;
                ev.surface_kind = classify(im.point.x, im.point.y, im.point.z);
                events.push_back(ev);
                ++st.impacts;
                if (!c.disabled && im.energy > 9000.0f && !c.emergency && !c.responder) wreck(c, ev);
            }
        }
        std::erase_if(cars, [&](Car& c) {
            if (!c.remove) return false;
            d.traffic.remove_vehicle(c.id);
            if (c.id == incident_car) incident_car = 0;
            ++st.despawned;
            return true;
        });
        maybe_spawn();
        incident_logic();
        const auto t3 = std::chrono::steady_clock::now();
        st.ms_traffic += std::chrono::duration<double, std::milli>(t2 - t1).count();
        st.ms_vehicles += std::chrono::duration<double, std::milli>(t3 - t2).count() + std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    [[nodiscard]] std::uint8_t classify(double x, double y, double z) const {
        for (const StreetProp& p : d.props) {
            if (std::fabs(p.x - x) < 3.2 && std::fabs(p.y - y) < 3.2) {
                Aabb b;
                if (!prop_box(p, b)) continue;
                if (x > b.min.x - 0.5 && x < b.max.x + 0.5 && y > b.min.y - 0.5 && y < b.max.y + 0.5 && z < b.max.z + 0.3) return p.type == "parked_car" ? 1 : 3;
            }
        }
        for (const Building& b : d.buildings)
            if (b.footprint.x0 - 0.6 < x && x < b.footprint.x1 + 0.6 && b.footprint.y0 - 0.6 < y && y < b.footprint.y1 + 0.6) return 2;
        return z < 0.5 ? 4 : 0;
    }

    [[nodiscard]] static double curve_param(const Curve& cv, double x, double y) {
        double best = 1e18, bt = 0.0;
        for (int i = 0; i <= 24; ++i) {
            const double t = i / 24.0;
            const auto q = cv.at(t);
            const double dd = (q[0] - x) * (q[0] - x) + (q[1] - y) * (q[1] - y);
            if (dd < best) {
                best = dd;
                bt = t;
            }
        }
        return bt;
    }

    [[nodiscard]] double pursuit_steer(const Car& c, const traffic::TrafficVehicle& tv, const VehicleState& vs) const {
        const double speed = std::max(0.0, static_cast<double>(vs.forward_speed));
        const double ld = std::clamp(2.8 + 0.4 * speed, 3.5, 9.0);
        // Walk `ld` metres ahead along the route from the car's position on its lane.
        const traffic::LaneSegment* lane = d.traffic.find_lane(tv.current_lane);
        double remaining = ld;
        double s = lane_s(*lane, vs.position.x, vs.position.y);
        if (const auto cu = curves.find(lane->id); cu != curves.end()) s = curve_param(cu->second, vs.position.x, vs.position.y) * lane->length_m();
        std::size_t k = c.cursor;
        double tx = 0, ty = 0;
        for (;;) {
            const double len = lane->length_m();
            if (s + remaining <= len || k + 1 >= c.route.size()) {
                const auto cu = curves.find(lane->id);
                if (cu != curves.end()) {
                    const auto q = cu->second.at(std::clamp((s + remaining) / len, 0.0, 1.0));
                    tx = q[0];
                    ty = q[1];
                    break;
                }
                const auto t = lane_tangent(*lane);
                const double ss = std::min(s + remaining, len + 4.0);
                tx = lane->start.x + t[0] * ss;
                ty = lane->start.y + t[1] * ss;
                break;
            }
            remaining -= std::max(0.0, len - s);
            ++k;
            const traffic::LaneSegment* next = d.traffic.find_lane(c.route[k]);
            if (next == nullptr) break;
            lane = next;
            s = 0.0;
        }
        const double alpha = wrap(std::atan2(ty - vs.position.y, tx - vs.position.x) - vs.yaw);
        const double wheelbase = 2.8;
        const double delta = std::atan2(2.0 * wheelbase * std::sin(alpha), ld);
        return std::clamp(delta / static_cast<double>(c.body->config().max_steer), -1.0, 1.0);
    }

    void wreck(Car& c, const TrafficEvent& cause) {
        c.disabled = true;
        c.disabled_at = time;
        c.distract_until = -1.0;
        TrafficEvent ev = cause;
        ev.type = TrafficEvent::Type::Accident;
        events.push_back(ev);
        ++st.accidents;
        if (incident_car == 0) {
            incident_car = c.id;
            incident_x = c.body->state().position.x;
            incident_y = c.body->state().position.y;
            const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
            incident_lane = tv != nullptr ? tv->current_lane : 0;
            response_sent = false;
        }
    }

    void incident_logic() {
        // The response: a short while after the crash an ambulance and a fire engine leave the station for the lane the wreck is in.
        if (incident_car != 0 && !response_sent) {
            Car* w = find(incident_car);
            if (w != nullptr && time - w->disabled_at > 6.0) {
                response_sent = true;
                dispatch(incident_x, incident_y);
            }
        }
        if (incident_car != 0) {
            Car* w = find(incident_car);
            if (w == nullptr) {
                incident_car = 0;
            } else {
                // On scene: a response vehicle stopped near the wreck.
                for (Car& c : cars) {
                    if (!c.emergency || c.id == w->id) continue;
                    const VehicleState& vs = c.body->state();
                    const double dist = std::hypot(vs.position.x - incident_x, vs.position.y - incident_y);
                    if (dist < 22.0 && std::fabs(vs.forward_speed) < 0.4) {
                        if (c.on_scene_since < 0.0) {
                            c.on_scene_since = time;
                            TrafficEvent ev;
                            ev.type = TrafficEvent::Type::EmergencyOnScene;
                            ev.vehicle = c.id;
                            ev.x = vs.position.x;
                            ev.y = vs.position.y;
                            events.push_back(ev);
                        }
                    }
                }
                // The road reopens once a responder has been on scene for 25 s (the others, still on their way, are sent back out with it).
                bool worked_long_enough = false;
                for (const Car& c : cars)
                    if (c.emergency && c.on_scene_since >= 0.0 && time - c.on_scene_since >= 25.0) worked_long_enough = true;
                if (response_sent && worked_long_enough) {
                    // The wreck is towed away and the road reopens; the responders go back.
                    TrafficEvent ev;
                    ev.type = TrafficEvent::Type::Cleared;
                    ev.vehicle = w->id;
                    ev.x = incident_x;
                    ev.y = incident_y;
                    events.push_back(ev);
                    ++st.cleared;
                    w->remove = true;
                    incident_car = 0;
                    for (Car& c : cars) {
                        if (!c.emergency) continue;
                        const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
                        if (tv == nullptr) continue;
                        const traffic::LaneId exit = exits[static_cast<std::size_t>(rng.pick(static_cast<int>(exits.size())))];
                        auto route = d.traffic.find_lane_route(tv->current_lane, exit);
                        if (!route.empty()) {
                            c.route = route;
                            c.cursor = 0;
                            c.emergency = false;   // from here it is an ordinary vehicle on its way out (siren off)
                        }
                    }
                }
            }
        }
        // Chance of an accident: one distracted driver at a time, never while the district is already dealing with one.
        if (accidents && incident_car == 0 && time >= next_accident_roll && cars.size() >= 6) {
            next_accident_roll = time + 4.0;
            if (rng.unit() < 4.0 / 80.0) {   // about one per 80 s of world time
                std::vector<Car*> candidates;
                for (Car& c : cars)
                    if (can_distract(c)) candidates.push_back(&c);
                if (!candidates.empty()) {
                    Car* c = candidates[static_cast<std::size_t>(rng.pick(static_cast<int>(candidates.size())))];
                    distract(c->id, rng.range(1.8, 2.6));
                }
            }
        }
    }

    // A driver can only lose attention on an ordinary road: not on the way in from the edge (nothing could be sent there), not in an intersection, not near the lane's end, not while already wrecked or responding.
    [[nodiscard]] bool can_distract(const Car& c) const {
        if (c.disabled || c.emergency || c.distract_until > 0.0) return false;
        const traffic::TrafficVehicle* tv = d.traffic.find_vehicle(c.id);
        if (tv == nullptr) return false;
        const traffic::LaneSegment* lane = d.traffic.find_lane(tv->current_lane);
        if (lane == nullptr || lane->end_intersection == traffic::kInvalidIntersectionId) return false;
        if (std::find(d.entry_lanes.begin(), d.entry_lanes.end(), lane->id) != d.entry_lanes.end()) return false;
        return c.body->state().forward_speed > 6.5f && tv->s_m > 4.0 && lane->length_m() - tv->s_m > 22.0;
    }

    bool distract(std::uint32_t id, double seconds) {
        Car* c = find(id);
        if (c == nullptr || !can_distract(*c)) return false;
        c->distract_until = time + seconds;
        c->distract_bias = -0.55;   // toward the right-hand kerb (negative steer is to the right)
        return true;
    }

    void dispatch(double x, double y) {
        // The fire station's door.
        const Building* station = nullptr;
        for (const Building& b : d.buildings)
            if (b.kind == BuildingKind::FireStation) station = &b;
        if (std::getenv("ALIVE_DEBUG")) std::fprintf(stderr, "dispatch: station %p incident_lane %u\n", static_cast<const void*>(station), static_cast<unsigned>(incident_lane));
        if (station == nullptr) return;
        const auto dir = leg_dir(station->front);
        const double px = station->door_x + dir[0] * 8.0, py = station->door_y + dir[1] * 8.0;
        // Target: the lane the wreck is in.
        traffic::LaneId target_lane = incident_lane;
        {
            // An entry lane (the road's way in from the edge) has no way to it: use the nearest lane that can be reached from the station, the opposing carriageway included.
            std::vector<std::pair<double, traffic::LaneId>> by_distance;
            for (const District::LanePath& lp : d.lane_paths) {
                if (lp.connector) continue;
                const traffic::LaneSegment* l = d.traffic.find_lane(lp.id);
                if (l == nullptr) continue;
                const double s = std::clamp(lane_s(*l, incident_x, incident_y), 0.0, l->length_m());
                const auto t = lane_tangent(*l);
                by_distance.push_back({std::hypot(l->start.x + t[0] * s - incident_x, l->start.y + t[1] * s - incident_y), lp.id});
            }
            std::sort(by_distance.begin(), by_distance.end());
            bool ok = false;
            for (std::size_t i = 0; i < by_distance.size() && i < 8 && !ok; ++i) {
                for (const District::LanePath& lp : d.lane_paths) {
                    if (lp.connector) continue;
                    if (!d.traffic.find_lane_route(lp.id, by_distance[i].second).empty() && std::find(d.entry_lanes.begin(), d.entry_lanes.end(), by_distance[i].second) == d.entry_lanes.end()) {
                        target_lane = by_distance[i].second;
                        ok = true;
                    }
                    if (ok) break;
                }
            }
        }
        // Candidate departure lanes: the nearest few non-connector lanes to the station's front.
        std::vector<std::pair<double, traffic::LaneId>> near;
        for (const District::LanePath& lp : d.lane_paths) {
            if (lp.connector) continue;
            const traffic::LaneSegment* l = d.traffic.find_lane(lp.id);
            if (l == nullptr) continue;
            const double s = std::clamp(lane_s(*l, px, py), 0.0, l->length_m());
            const auto t = lane_tangent(*l);
            near.push_back({std::hypot(l->start.x + t[0] * s - px, l->start.y + t[1] * s - py), lp.id});
        }
        std::sort(near.begin(), near.end());
        (void)x;
        (void)y;
        const VehicleKind kinds[2] = {VehicleKind::Ambulance, VehicleKind::FireEngine};
        int sent = 0;
        for (const VehicleKind k : kinds) {
            for (std::size_t i = 0; i < near.size() && i < 6; ++i) {
                const traffic::LaneId start = near[i].second;
                const traffic::LaneSegment* l = d.traffic.find_lane(start);
                if (!lane_free_at_start(start) && lane_s(*l, px, py) < 16.0) continue;
                auto route = d.traffic.find_lane_route(start, target_lane);
                if (std::getenv("ALIVE_DEBUG")) std::fprintf(stderr, "  try lane %u -> %u route %zu\n", static_cast<unsigned>(start), static_cast<unsigned>(target_lane), route.size());
                if (route.empty()) continue;
                // Append a way out after the scene so the route is complete.
                const double s0 = std::clamp(lane_s(*l, px, py), 1.0, std::max(1.0, l->length_m() - 3.0));
                if (spawn(k, route, k == VehicleKind::FireEngine ? std::max(1.0, s0 - 11.0) : s0, true)) {
                    TrafficEvent ev;
                    ev.type = TrafficEvent::Type::EmergencyDispatched;
                    ev.vehicle = cars.back().id;
                    ev.x = px;
                    ev.y = py;
                    events.push_back(ev);
                    ++st.dispatches;
                    ++sent;
                    break;
                }
            }
        }
        (void)sent;
    }
};

TrafficDriver::TrafficDriver(District& d, std::uint32_t seed, int target_vehicles, bool accidents_enabled) : impl_(std::make_unique<Impl>(d, seed, target_vehicles, accidents_enabled)) {}
TrafficDriver::~TrafficDriver() = default;
void TrafficDriver::step(double dt) {
    // The caller steps in fixed 1/60 s; any other dt is split.
    double left = dt;
    while (left > 1e-9) {
        impl_->substep();
        left -= kStep;
    }
}
double TrafficDriver::time() const { return impl_->time; }
std::vector<TrafficEvent> TrafficDriver::take_events() {
    std::vector<TrafficEvent> out;
    out.swap(impl_->events);
    return out;
}
void TrafficDriver::dispatch_emergency(double x, double y) { impl_->dispatch(x, y); }
bool TrafficDriver::distract(std::uint32_t id, double seconds) { return impl_->distract(id, seconds); }

TrafficStats TrafficDriver::stats() const {
    TrafficStats s = impl_->st;
    s.vehicles = static_cast<std::uint32_t>(impl_->cars.size());
    for (const Car& c : impl_->cars) {
        if (std::fabs(c.body->state().forward_speed) > 0.3f) ++s.moving;
        if (c.emergency) ++s.emergency;
        if (c.disabled) ++s.disabled;
    }
    s.stopped_at_signal = 0;
    for (const Car& c : impl_->cars) {
        const traffic::TrafficVehicle* tv = impl_->d.traffic.find_vehicle(c.id);
        if (tv != nullptr && tv->control_output.brake_reason == traffic::BrakeReason::RedSignal && std::fabs(c.body->state().forward_speed) < 0.5f) ++s.stopped_at_signal;
    }
    return s;
}

std::vector<VehicleView> TrafficDriver::vehicles() const {
    std::vector<VehicleView> out;
    for (const Car& c : impl_->cars) {
        const VehicleState& vs = c.body->state();
        VehicleView v;
        v.id = c.id;
        v.kind = c.kind;
        v.x = vs.position.x;
        v.y = vs.position.y;
        v.z = vs.position.z;
        v.q = {vs.orientation.x, vs.orientation.y, vs.orientation.z, vs.orientation.w};
        v.yaw = vs.yaw;
        v.speed = vs.forward_speed;
        for (std::size_t i = 0; i < 4; ++i) {
            v.wheel_spin[i] = vs.wheels[i].spin;
            v.wheel_compression[i] = vs.wheels[i].compression;
        }
        v.wheel_steer = {vs.wheels[0].steer_angle, vs.wheels[1].steer_angle};
        v.braking = c.braking;
        const traffic::TrafficVehicle* tv = impl_->d.traffic.find_vehicle(c.id);
        v.siren = tv != nullptr && tv->control_output.siren_active;
        v.disabled = c.disabled;
        const VehicleDamageState& dm = c.body->damage();
        v.damage = *std::max_element(dm.zone.begin(), dm.zone.end());
        v.zones = dm.zone;
        v.palette = c.palette;
        v.engine_rpm = static_cast<int>(vs.engine_rpm);
        out.push_back(v);
    }
    return out;
}

std::vector<std::array<double, 5>> TrafficDriver::obstacles() const {
    std::vector<std::array<double, 5>> out;
    for (const Car& c : impl_->cars) {
        const VehicleState& vs = c.body->state();
        out.push_back({vs.position.x, vs.position.y, vs.velocity.x, vs.velocity.y, 0.5 * Impl::length_of(c.kind)});
    }
    return out;
}

} // namespace saige::alive
