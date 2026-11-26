#include "director.hpp"

#include "district_validate.hpp"
#include "vegetation_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace saige::alive::rend {

namespace {

struct V {
    double x = 0, y = 0, z = 0;
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V lerp(V a, V b, double t) { return a + (b - a) * t; }
double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
Float3 f3(V v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

// A critically damped follower (what a camera operator's smoothing does).
struct Damp {
    V pos, vel;
    bool init = false;
    V update(V target, double smooth_time, double dt) {
        if (!init) {
            pos = target;
            vel = {};
            init = true;
            return pos;
        }
        const double omega = 2.0 / std::max(1e-3, smooth_time);
        const double x = omega * dt;
        const double e = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
        const V change = pos - target;
        const V temp = (vel + change * omega) * dt;
        vel = (vel - temp * omega) * e;
        pos = target + (change + temp) * e;
        return pos;
    }
    void reset() { init = false; }
};

// The most the eye may be moved to clear foliage or an awning (metres): enough to step off a 3.5 m sidewalk into the parking lane, not enough to lose the subject.
constexpr float kMaxEyeShift = 5.0f;
constexpr float kNearWall = 1.2f;   // a wall closer than this straight ahead of the eye is a view of a surface, not of the scene

struct Shot {
    double t0 = 0, t1 = 0;
    std::string name;
    std::function<Cam(Shot&, double u, double dt, const World&)> fn;
    // per-shot state
    std::uint64_t subject = 0;
    bool tracks_subject = false;
    bool has_focus = false;        // a place the shot is about (the wreck): the camera must see it, as it must see a followed subject
    V focus{};   // follows a resident or a vehicle (a place-based shot, an establishing or aerial one, has no subject to hide)
    bool chosen = false;
    Damp eye, look;
};

const PedView* find_ped(const std::vector<PedView>& peds, std::uint64_t id) {
    for (const PedView& p : peds)
        if (p.id == id) return &p;
    return nullptr;
}
const VehicleView* find_vehicle(const std::vector<VehicleView>& vs, std::uint32_t id) {
    for (const VehicleView& v : vs)
        if (v.id == id) return &v;
    return nullptr;
}

V ped_pos(const PedView& p) { return {p.x, p.y, p.z}; }

} // namespace

struct Director::Impl {
    const District& d;
    ScoutReport rep;
    double length = 180.0;
    double preroll = 25.0;
    std::vector<Shot> shots;

    Impl(const District& dist, const ScoutReport& r, double film, double pre) : d(dist), rep(r), length(film), preroll(pre) { build(); }

    // ---- shot builders ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    static Shot dolly(double t0, double t1, const char* name, V eye0, V eye1, V look0, V look1, double fov0, double fov1) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
        s.fn = [=](Shot&, double u, double, const World&) {
            const double k = smooth(u);
            return Cam{f3(lerp(eye0, eye1, k)), f3(lerp(look0, look1, k)), static_cast<float>(fov0 + (fov1 - fov0) * k)};
        };
        return s;
    }

    // Behind and above a walking resident, the way a third-person game frames its character.
    static Shot follow_ped(double t0, double t1, const char* name, V near, double back, double height, double side, bool inside = false) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
        s.tracks_subject = true;
        s.fn = [=](Shot& sh, double, double dt, const World& w) {
            const std::vector<PedView> peds = w.pedestrians();
            if (!sh.chosen) {
                double best = 1e18;
                for (const PedView& p : peds) {
                    if (!p.visible || p.speed < 1.0 || p.inside_building != inside) continue;
                    const double dd = len(ped_pos(p) - near);
                    if (dd < best) {
                        best = dd;
                        sh.subject = p.id;
                    }
                }
                sh.chosen = true;
            }
            const PedView* p = find_ped(peds, sh.subject);
            if (p == nullptr) return Cam{f3({near.x, near.y, 3.0}), f3(near), 55.0f};
            const V pos = ped_pos(*p);
            const V fwd{std::cos(p->yaw), std::sin(p->yaw), 0};
            const V left{-fwd.y, fwd.x, 0};
            const V eye = sh.eye.update(pos - fwd * back + left * side + V{0, 0, height}, 0.55, dt);
            const V look = sh.look.update(pos + fwd * 1.5 + V{0, 0, 1.25}, 0.30, dt);
            Cam c{f3(eye), f3(look), 52.0f};
            c.subject = f3(pos + V{0, 0, 1.0});
            c.has_subject = true;
            return c;
        };
        return s;
    }

    // Chasing a moving car (or sitting at its bumper or windscreen: `mode` 0 chase, 1 bumper, 2 driver).
    static Shot follow_car(double t0, double t1, const char* name, V near, int mode, double back = 7.5, double height = 2.3, double side = 0.8) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
        s.tracks_subject = true;
        s.fn = [=](Shot& sh, double, double dt, const World& w) {
            const std::vector<VehicleView> vs = w.vehicles();
            if (!sh.chosen) {
                double best = 1e18;
                for (const VehicleView& v : vs) {
                    if (v.kind != VehicleKind::Sedan && v.kind != VehicleKind::Taxi && v.kind != VehicleKind::Hatchback) continue;
                    if (v.speed < 5.0 || std::fabs(v.x) > 105.0 || v.y < -100.0 || v.y > 55.0) continue;   // in the district, not out on a boundary road
                    const double dd = len(V{v.x, v.y, 0} - V{near.x, near.y, 0});
                    if (dd < best) {
                        best = dd;
                        sh.subject = v.id;
                    }
                }
                sh.chosen = true;
            }
            const VehicleView* v = find_vehicle(vs, static_cast<std::uint32_t>(sh.subject));
            if (v == nullptr) return Cam{f3({near.x, near.y, 4.0}), f3({near.x, near.y + 8, 0.5}), 55.0f};
            const V pos{v->x, v->y, v->z};
            const V fwd{std::cos(v->yaw), std::sin(v->yaw), 0};
            const V left{-fwd.y, fwd.x, 0};
            if (mode == 2) {
                const V eye = pos + fwd * 0.15 + left * 0.38 + V{0, 0, 0.62};
                return Cam{f3(eye), f3(eye + fwd * 14.0 + V{0, 0, -0.5}), 68.0f};
            }
            if (mode == 1) {
                const V eye = pos + fwd * 2.4 + V{0, 0, 0.0 - 0.1};
                const V e2 = sh.eye.update(eye, 0.08, dt);
                return Cam{f3(e2), f3(e2 + fwd * 12.0 + V{0, 0, 0.6}), 62.0f};
            }
            const V eye = sh.eye.update(pos - fwd * back + left * side + V{0, 0, height}, 0.45, dt);
            const V look = sh.look.update(pos + fwd * 6.0 + V{0, 0, 0.5}, 0.25, dt);
            Cam c{f3(eye), f3(look), 55.0f};
            c.subject = f3(pos + V{0, 0, 0.8});
            c.has_subject = true;
            return c;
        };
        return s;
    }

    static Shot follow_vehicle_id(double t0, double t1, const char* name, std::uint32_t id, double back, double height, double side, V fallback) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
        s.tracks_subject = true;
        s.fn = [=](Shot& sh, double, double dt, const World& w) {
            const std::vector<VehicleView> vs = w.vehicles();
            const VehicleView* v = find_vehicle(vs, id);
            if (v == nullptr) return Cam{f3(fallback + V{0, 0, 3}), f3(fallback), 55.0f};
            const V pos{v->x, v->y, v->z};
            const V fwd{std::cos(v->yaw), std::sin(v->yaw), 0};
            const V left{-fwd.y, fwd.x, 0};
            const V eye = sh.eye.update(pos - fwd * back + left * side + V{0, 0, height}, 0.5, dt);
            const V look = sh.look.update(pos + fwd * 8.0 + V{0, 0, 0.6}, 0.3, dt);
            Cam c{f3(eye), f3(look), 52.0f};
            c.subject = f3(pos + V{0, 0, 0.8});
            c.has_subject = true;
            return c;
        };
        return s;
    }

    static Shot hold(double t0, double t1, const char* name, V eye, V look, double fov) { return dolly(t0, t1, name, eye, eye, look, look, fov, fov); }

    void build() {
        const double T = length;
        // The cafe of this district (its position depends on the seed).
        V door{-19.8, 51.0, 0}, out{0, 1, 0}, along{-1, 0, 0};
        for (const Building& b : d.buildings)
            if (b.kind == BuildingKind::Cafe && b.interior) {
                door = {b.door_x, b.door_y, 0};
                const auto dir = leg_dir(b.front);
                out = {dir[0], dir[1], 0};
                along = {-dir[1], dir[0], 0};
            }
        // An accident in the first seconds of the film (or before it) has no run-up to cut to: the film is then shot as one without an accident block rather than with shots that start before the film does.
        const bool has_accident = rep.accident_t > 0.0 && rep.accident_t - preroll >= 2.5;
        const double ta = has_accident ? rep.accident_t - preroll : -1.0;   // film time of the accident
        // Intro: ordinary life, scaled to end 9 s before the accident (or to take 60% of the film without one).
        struct Base {
            const char* name;
            double dur;
        };
        // The intro ends 9 s before the accident, and lasts at least 40 s when the accident leaves room for that. An early accident (before 49 s) used to keep the full 40 s intro anyway, so the shots of the
        // accident block started in the middle of it and the sorted timeline interleaved the two (seeds 2 and 6: the wreck, then the crossing, then the street reacting). The intro now ends where the run-up does and keeps only the
        // opening shots that fit at 80% of their natural length or more.
        const double intro_end = has_accident ? (ta - 9.0 >= 40.0 ? ta - 9.0 : std::min(std::max(ta - 9.0, 6.0), ta - 1.0)) : T * 0.6;
        std::vector<Base> intro = {{"harbor", 13}, {"cafe exterior", 11}, {"crossing", 13}, {"chase", 13}, {"follow", 11}, {"cafe interior", 9}, {"driver", 8}};
        if (has_accident && intro_end < 40.0) {
            double acc = 0;
            std::size_t keep = 0;
            for (const Base& b : intro) {
                if (keep > 0 && acc + b.dur * 0.8 > intro_end) break;
                acc += b.dur;
                ++keep;
            }
            intro.resize(keep);
        }
        double sum = 0;
        for (const Base& b : intro) sum += b.dur;
        const double k = intro_end / sum;
        double t = 0;
        const auto next = [&](const Base& b) {
            const double t0 = t;
            t += b.dur * k;
            return std::pair<double, double>{t0, t};
        };
        {
            const auto [a, b] = next(intro[0]);
            shots.push_back(dolly(a, b, "harbor establishing", {30, 200, 36}, {-14, 118, 13}, {-4, 45, 9}, {-12, 40, 5}, 50, 56));
        }
        if (intro.size() > 1) {
            const auto [a, b] = next(intro[1]);
            shots.push_back(dolly(a, b, "cafe terrace", door + out * 12.0 - along * 14.0 + V{0, 0, 1.75}, door + out * 11.5 + along * 9.0 + V{0, 0, 1.8}, door + out * 1.2 + V{0, 0, 1.6}, door + out * 1.2 + along * 1.5 + V{0, 0, 1.6}, 52, 46));
        }
        if (intro.size() > 2) {
            const auto [a, b] = next(intro[2]);
            shots.push_back(dolly(a, b, "crossing at the main intersection", {12.5, -14.5, 3.3}, {8.8, -10.8, 2.5}, {0, -1.5, 1.6}, {0, -0.5, 1.4}, 56, 46));
        }
        if (intro.size() > 3) {
            const auto [a, b] = next(intro[3]);
            shots.push_back(follow_car(a, b, "chase cam", {0, -30, 0}, 0));
        }
        if (intro.size() > 4) {
            const auto [a, b] = next(intro[4]);
            shots.push_back(follow_ped(a, b, "follow a resident", {-90, 30, 0}, 3.4, 1.9, 0.4));
        }
        if (intro.size() > 5) {
            const auto [a, b] = next(intro[5]);
            shots.push_back(dolly(a, b, "inside the cafe", door - out * 1.6 - along * 6.0 + V{0, 0, 1.75}, door - out * 1.8 - along * 4.0 + V{0, 0, 1.7}, door - out * 8.0 + along * 3.0 + V{0, 0, 1.2}, door - out * 8.0 + along * 2.0 + V{0, 0, 1.3}, 78, 72));
        }
        if (intro.size() > 6) {
            const auto [a, b] = next(intro[6]);
            shots.push_back(follow_car(a, b, "driver's view", {40, 0, 0}, 2));
        }
        double cursor = t;
        if (has_accident) {
            const double acc_end = ta + 62.0;
            // 1) the doomed car from behind, 2) the wreck close, 3) the street's reaction, 4) the response arriving, 5) at the scene.
            const V at{rep.accident_x, rep.accident_y, 1.0};
            shots.push_back(follow_vehicle_id(cursor, ta + 0.9, "traffic: a car drifts", rep.accident_vehicle, 11.0, 3.2, 1.2, at));
            // The camera for the wreck: off to the side of the road, looking at the car (horizontal offset towards the road's open side).
            const bool on_avenue = std::fabs(at.x + 90.0) < 12.0 || std::fabs(at.x) < 12.0 || std::fabs(at.x - 90.0) < 12.0;
            // Where each camera stands is chosen where the accident really is. The three shots of the block (the wreck, the street reacting from the opposite side, the scene) each take, independently, the first direction
            // for which the eyes at the start, middle and end are out of walls, not staring at one and not behind a building: the usual direction first, then every 10 degrees either side of it; at full distance first, then closer
            // (down to 0.3 times: the vector's length scales every distance of its shot). Where none works the usual direction stays and the clearance search handles the frames (and reports what it could not).
            const V side0 = on_avenue ? V{1.0, 0.0, 0.0} : V{0.0, 1.0, 0.0};   // unit, horizontal
            const V focus_pt = at + V{0, 0, 0.8};
            // The eyes of group g (0 the wreck, 1 the street reacts, 2 the scene) for a direction vector s: start and end of the dolly.
            const auto group_eyes = [&](int g, const V s) {
                const V pp{-s.y, s.x, 0.0};
                const auto at_s = [&](double dist, double height) { return at + s * dist + V{0, 0, height}; };
                if (g == 0) return std::array<V, 2>{at_s(8.0, 1.7) + pp * 7.0, at_s(5.5, 1.5) + pp * 5.0};
                if (g == 1) return std::array<V, 2>{at_s(-12.0, 1.8) - pp * 9.0, at_s(-9.0, 1.9) - pp * 6.0};
                const V slide = on_avenue ? V{0, -4, 0} : V{-4, 0, 0};
                return std::array<V, 2>{at_s(12.0, 1.8) + slide, at_s(8.0, 2.4) - slide};
            };
            const auto eye_ok = [&](const V e) {
                constexpr float m = 0.35f;
                const Float3 ef = f3(e);
                for (const Aabb& b : d.collision)
                    if (b.max.z >= 1.8f && ef.x > b.min.x - m && ef.x < b.max.x + m && ef.y > b.min.y - m && ef.y < b.max.y + m && ef.z < b.max.z + m && ef.z > b.min.z - m) return false;
                const std::array<float, 3> ea{ef.x, ef.y, ef.z}, la{static_cast<float>(focus_pt.x), static_cast<float>(focus_pt.y), static_cast<float>(focus_pt.z)};
                return !view_into_solid(d, ea, la, kNearWall) && !sightline_blocked_by_building(d, ea, la);
            };
            const auto choose = [&](int g) {
                for (int step_k = 0; step_k <= 18; ++step_k)
                    for (const int sign : {1, -1}) {
                        if (step_k == 0 && sign < 0) continue;
                        const double ang = static_cast<double>(sign * step_k * 10) * 3.14159265358979323846 / 180.0;   // 0, +-10, ... +-180 degrees
                        for (const double sc : {1.0, 0.85, 0.7, 0.55, 0.4, 0.3}) {
                            const V cand{(side0.x * std::cos(ang) - side0.y * std::sin(ang)) * sc, (side0.x * std::sin(ang) + side0.y * std::cos(ang)) * sc, 0.0};
                            const auto eyes = group_eyes(g, cand);
                            bool ok = true;
                            for (const double u : {0.0, 0.5, 1.0})
                                if (!eye_ok(lerp(eyes[0], eyes[1], u))) ok = false;
                            if (ok) return cand;
                        }
                    }
                return side0;
            };
            const std::array<V, 3> sides{choose(0), choose(1), choose(2)};
            const auto cam_at = [&](int g, double dist, double height) { return at + sides[static_cast<std::size_t>(g)] * dist + V{0, 0, height}; };
            shots.push_back(dolly(ta + 0.9, ta + 9.0, "the wreck", group_eyes(0, sides[0])[0], group_eyes(0, sides[0])[1], at + V{0, 0, 0.7}, at + V{0, 0, 0.9}, 50, 42));
            shots.back().tracks_subject = true;   // the place the shot is about must be in view, as a followed subject must
            shots.back().has_focus = true;
            shots.back().focus = at + V{0, 0, 0.8};
            shots.push_back(dolly(ta + 9.0, ta + 22.0, "the street reacts", group_eyes(1, sides[1])[0], group_eyes(1, sides[1])[1], at + V{0, 0, 1.0}, at + V{0, 0, 1.2}, 58, 50));
            shots.back().tracks_subject = true;   // the place the shot is about must be in view, as a followed subject must
            shots.back().has_focus = true;
            shots.back().focus = at + V{0, 0, 0.8};
            // The response: the first dispatched vehicle (ambulance).
            std::uint32_t amb = 0;
            for (const ScoutEvent& e : rep.events)
                if (e.type == TrafficEvent::Type::EmergencyDispatched) {
                    amb = e.vehicle;
                    break;
                }
            const double on = rep.onscene_t > 0 ? rep.onscene_t - preroll : ta + 45.0;
            const double resp0 = ta + 22.0;
            if (amb != 0) {
                shots.push_back(follow_vehicle_id(resp0, std::max(resp0 + 6.0, (resp0 + on - 4.0) * 0.5 + 6.0), "the ambulance", amb, 9.0, 3.4, 1.8, at));
                shots.push_back(follow_vehicle_id(std::max(resp0 + 6.0, (resp0 + on - 4.0) * 0.5 + 6.0), on - 3.0, "responding through the signals", amb, -6.0, 2.0, 2.2, at));   // from ahead, looking back at it
            } else {
                shots.push_back(hold(resp0, on - 3.0, "street", cam_at(1, -16.0, 2.5), at, 55));
            }
            shots.push_back(dolly(on - 3.0, acc_end, "at the scene", group_eyes(2, sides[2])[0], group_eyes(2, sides[2])[1], at + V{0, 0, 1.2}, at + V{0, 0, 1.4}, 55, 48));
            shots.back().tracks_subject = true;   // the place the shot is about must be in view, as a followed subject must
            shots.back().has_focus = true;
            shots.back().focus = at + V{0, 0, 0.8};
            cursor = acc_end;
        }
        // Finale: the evening settles over the district: a slow pull back and up with the windows lit.
        shots.push_back(dolly(cursor, T, "dusk over the district", {75, -10, 34}, {125, -150, 70}, {0, 15, 6}, {0, 25, 4}, 52, 58));
        std::sort(shots.begin(), shots.end(), [](const Shot& a, const Shot& b) { return a.t0 < b.t0; });
        // Make the shots contiguous.
        for (std::size_t i = 0; i + 1 < shots.size(); ++i) shots[i].t1 = shots[i + 1].t0;
        shots.back().t1 = T;
    }

    Shot* shot_at(double t) {
        for (Shot& s : shots)
            if (t >= s.t0 && t < s.t1) return &s;
        return shots.empty() ? nullptr : &shots.back();
    }

    // Keep the eye out of solid geometry (buildings, props): pushed to the nearest face.
    Float3 clear_of_geometry(Float3 eye) const {
        for (int pass = 0; pass < 3; ++pass)
            for (const Aabb& b : d.collision) {
                if (b.max.z < 0.9f) continue;
                const float m = 0.35f;
                if (eye.x > b.min.x - m && eye.x < b.max.x + m && eye.y > b.min.y - m && eye.y < b.max.y + m && eye.z < b.max.z + m && eye.z > b.min.z - m) {
                    const float dl = eye.x - (b.min.x - m), dr = (b.max.x + m) - eye.x, db = eye.y - (b.min.y - m), dt2 = (b.max.y + m) - eye.y, du = (b.max.z + m) - eye.z;
                    const float mn = std::min({dl, dr, db, dt2, du});
                    if (mn == du) eye.z = b.max.z + m;
                    else if (mn == dl) eye.x = b.min.x - m;
                    else if (mn == dr) eye.x = b.max.x + m;
                    else if (mn == db) eye.y = b.min.y - m;
                    else eye.y = b.max.y + m;
                }
            }
        return eye;
    }

    std::vector<StreetProp> trees;
    std::vector<Box3> awnings;

    // ---- the eye's clearance --------------------------------------------------------------------------------------------------------------------------------------------------------------
    // A shot proposes an eye; the eye is then judged by the same three rules the film's camera check applies (inside a wall or prop, looking through foliage or an awning, a building between the
    // eye and a subject the shot follows) plus a 0.35 m margin from solids. If the proposal fails, ONE search over offsets finds the nearest eye that passes: ring radius up to kMaxEyeShift
    // (horizontal) and up to kMaxEyeLift above the proposal, so that the displacement from what the shot asked for is bounded in total, whatever the rules and however many frames. (The earlier
    // version applied separate push-outs one after another, each with its own budget measured from where the previous one left the eye, so nothing bounded the sum, and a push out of one wall could
    // land in the neighbouring building: seed 3 had 279 frames with the eye inside a wall.) Between frames the offset is kept while it stays valid and relaxes back toward zero, so the camera does not
    // jump between candidates. A frame for which no candidate passes is counted (unresolved_frames) and left at the least-bad candidate: it is reported by the film's camera check, never hidden.
    static constexpr float kMaxEyeLift = 2.5f;
    static constexpr float kExtendedShift = 12.0f;
    static constexpr float kMaxReframe = 2.0944f;   // 120 degrees either side of the proposed bearing
    struct EyeFit {
        bool solid = false, obstructed = false, hidden = false;
        [[nodiscard]] int score() const { return (solid ? 4 : 0) + (obstructed ? 2 : 0) + (hidden ? 1 : 0); }
    };
    EyeFit judge(Float3 eye, Float3 look, Float3 target, float fov_deg, bool tracks_subject) const {
        EyeFit f;
        constexpr float m = 0.35f;
        for (const Aabb& b : d.collision) {
            if (b.max.z < 1.8f) continue;   // lower than an eye (a parked car, a bench, a kerb) is not a wall; the same threshold as eye_in_solid
            if (eye.x > b.min.x - m && eye.x < b.max.x + m && eye.y > b.min.y - m && eye.y < b.max.y + m && eye.z < b.max.z + m && eye.z > b.min.z - m) {
                f.solid = true;
                break;
            }
        }
        if (!f.solid && view_into_solid(d, {eye.x, eye.y, eye.z}, {look.x, look.y, look.z}, kNearWall)) f.solid = true;   // staring at a wall from a metre away is as bad as being in it
        f.obstructed = view_obstructed(trees, awnings, {eye.x, eye.y, eye.z}, {look.x, look.y, look.z}, fov_deg);
        f.hidden = tracks_subject && sightline_blocked_by_building(d, {eye.x, eye.y, eye.z}, {target.x, target.y, target.z});
        return f;
    }
    Float3 resolve_eye(Float3 eye0, Float3 look, Float3 target, float fov_deg, bool tracks_subject, const Shot* shot) {
        first_in_shot = shot != last_shot;
        if (first_in_shot) {   // a cut: the offset starts afresh and its first value is not a camera movement
            last_shot = shot;
            prev_off = {0, 0, 0};
        }
        const auto at = [&](const Float3& o) { return Float3{eye0.x + o.x, eye0.y + o.y, eye0.z + o.z}; };
        const auto dist = [](const Float3& a, const Float3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
        const auto fit_at = [&](const Float3& off) { return judge(at(off), look, target, fov_deg, tracks_subject); };
        const Float3 zero{0, 0, 0};
        const bool standing = prev_off.x != 0 || prev_off.y != 0 || prev_off.z != 0;
        if (fit_at(zero).score() == 0) {
            // The proposal itself is fine. Relax a standing offset toward it smoothly if the relaxed eye is fine too, otherwise keep the offset that is.
            if (!standing) return eye0;
            Float3 relaxed{prev_off.x * 0.96f, prev_off.y * 0.96f, prev_off.z * 0.96f};
            if (dist(relaxed, zero) < 0.02f) relaxed = zero;
            if (fit_at(relaxed).score() == 0) return commit(relaxed), at(relaxed);
            if (fit_at(prev_off).score() == 0) return commit(prev_off), at(prev_off);
        } else if (standing && fit_at(prev_off).score() == 0) {
            return commit(prev_off), at(prev_off);   // the standing offset still works: keep it (no jump)
        }
        // Search. Cost = 3 x the change from the standing offset + the distance from the proposal (a forced move is as short as the geometry allows, and the camera does not hop between candidates).
        //   tier 1: offsets within the budget (kMaxEyeShift horizontal, kMaxEyeLift up), every rule;
        //   tier 2: the same rules out to kExtendedShift: counted as beyond the budget (it is what leaves a wall that tier 1 cannot);
        //   tier 3 (a shot about a subject or a place): the eye on an arc around that subject at 0.7 to 1.3 times its distance, up to kMaxReframe either side of the proposed bearing: the subject keeps its distance and the camera
        //           changes its viewing angle instead of travelling to a place where the subject is hidden. Counted as re-framed;
        //   otherwise the least-bad candidate (a wall counts more than leaves, leaves more than a hidden subject) is kept and the frame is counted as unresolved.
        static const float budget = std::getenv("SAIGE_ALIVE_EYE_BUDGET") ? static_cast<float>(std::atof(std::getenv("SAIGE_ALIVE_EYE_BUDGET"))) : kMaxEyeShift;
        static constexpr float lifts[] = {0.0f, 0.75f, 1.5f, kMaxEyeLift};
        constexpr int kAngles = 24;
        struct Cand { Float3 off; float cost; int score; };
        Cand least_bad{prev_off, 1e30f, 1 << 20};
        const auto note_bad = [&](const Float3& off, int score, float cost) {
            if (score < least_bad.score || (score == least_bad.score && cost < least_bad.cost)) least_bad = {off, cost, score};
        };
        const auto search = [&](float limit) {
            Cand best{prev_off, 1e30f, 0};
            bool found = false;
            const auto consider = [&](const Float3& off) {
                if (std::hypot(off.x, off.y) > limit + 1e-3f || off.z > kMaxEyeLift + 1e-3f || off.z < -1e-3f) return;
                const EyeFit fit = fit_at(off);
                const float cost = 3.0f * dist(off, prev_off) + dist(off, zero);
                if (fit.score() == 0) {
                    if (!found || cost < best.cost) { best = {off, cost, 0}; found = true; }
                } else {
                    note_bad(off, fit.score(), cost);
                }
            };
            static constexpr float zero_rings[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 10.0f, 12.0f};
            for (const float r : zero_rings) {
                if (r > limit + 1e-3f) break;
                for (const float lift : lifts)
                    for (int k = 0; k < kAngles; ++k) {
                        const float ang = 6.2831853f * static_cast<float>(k) / static_cast<float>(kAngles);
                        consider({r * std::cos(ang), r * std::sin(ang), lift});
                    }
            }
            consider(prev_off);
            if (standing) {
                static constexpr float near_rings[] = {0.1f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
                for (const float r : near_rings)
                    for (const float dz : {0.0f, 0.5f, -0.5f})
                        for (int k = 0; k < 16; ++k) {
                            const float ang = 6.2831853f * static_cast<float>(k) / 16.0f;
                            consider({prev_off.x + r * std::cos(ang), prev_off.y + r * std::sin(ang), prev_off.z + dz});
                        }
            }
            return std::pair<Cand, bool>{best, found};
        };
        {
            const auto [best, found] = search(budget);
            if (found) return commit(best.off), at(best.off);
        }
        {
            const auto [best, found] = search(kExtendedShift);
            if (found) return commit(best.off), at(best.off);
        }
        if (tracks_subject) {
            // An arc around the subject. The bearing is measured from the subject to the proposed eye.
            const float bx = eye0.x - target.x, by = eye0.y - target.y;
            const float r0 = std::max(2.0f, std::hypot(bx, by));
            const float th0 = std::atan2(by, bx);
            Cand best{prev_off, 1e30f, 0};
            bool found = false;
            for (int step = 1; step <= 24; ++step) {
                for (const float sign : {1.0f, -1.0f}) {
                    const float dth = sign * static_cast<float>(step) * (kMaxReframe / 24.0f);
                    for (const float ratio : {1.0f, 0.85f, 0.7f, 1.15f, 1.3f})
                        for (const float lift : {0.0f, 0.75f, 1.5f}) {
                            const Float3 eye{target.x + r0 * ratio * std::cos(th0 + dth), target.y + r0 * ratio * std::sin(th0 + dth), eye0.z + lift};
                            const Float3 off{eye.x - eye0.x, eye.y - eye0.y, eye.z - eye0.z};
                            const EyeFit fit = judge(eye, look, target, fov_deg, tracks_subject);
                            const float cost = std::fabs(dth) * 2.0f + std::fabs(ratio - 1.0f) * 4.0f + 3.0f * dist(off, prev_off) * 0.1f;
                            if (fit.score() == 0) {
                                if (!found || cost < best.cost) { best = {off, cost, 0}; found = true; }
                            } else {
                                note_bad(off, fit.score(), cost + 100.0f);
                            }
                        }
                }
                if (found && static_cast<float>(step) * (kMaxReframe / 24.0f) * 2.0f > best.cost) break;   // no larger bearing change can cost less
            }
            if (found) {
                ++stats.reframed;
                commit(best.off, true);
                return at(best.off);
            }
        }
        ++stats.unresolved;
        commit(least_bad.off);
        return at(least_bad.off);
    }
    void commit(const Float3& off, bool reframed = false) {
        const float step = std::sqrt((off.x - prev_off.x) * (off.x - prev_off.x) + (off.y - prev_off.y) * (off.y - prev_off.y) + (off.z - prev_off.z) * (off.z - prev_off.z));
        if (!first_in_shot) stats.max_offset_step = std::max(stats.max_offset_step, step);
        if (!first_in_shot && step > 1.5f) ++stats.pops;
        if (!first_in_shot && step > 1.0f && std::getenv("SAIGE_ALIVE_CAMERA_DEBUG") != nullptr)
            std::printf("  offset jump %.2f m in shot '%s': (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", static_cast<double>(step), last_shot != nullptr ? last_shot->name.c_str() : "", static_cast<double>(prev_off.x), static_cast<double>(prev_off.y), static_cast<double>(prev_off.z), static_cast<double>(off.x), static_cast<double>(off.y), static_cast<double>(off.z));
        if (std::getenv("SAIGE_ALIVE_CAMERA_DEBUG") != nullptr && (std::hypot(off.x, off.y) > kMaxEyeShift + 1e-3f || reframed) && last_shot != nullptr) {
            static std::map<std::string, int> counts;
            if (counts[last_shot->name + (reframed ? " (reframed)" : " (beyond budget)")]++ == 0)
                std::printf("  beyond budget in shot '%s'%s: offset (%.1f %.1f %.1f)\n", last_shot->name.c_str(), reframed ? " (re-framed)" : "", static_cast<double>(off.x), static_cast<double>(off.y), static_cast<double>(off.z));
        }
        if (!reframed) stats.max_offset_horizontal = std::max(stats.max_offset_horizontal, std::hypot(off.x, off.y));
        stats.max_offset_lift = std::max(stats.max_offset_lift, off.z);
        if (off.x != 0 || off.y != 0 || off.z != 0) ++stats.displaced_frames;
        if (!reframed && (std::hypot(off.x, off.y) > kMaxEyeShift + 1e-3f || off.z > kMaxEyeLift + 1e-3f)) ++stats.over_budget;   // every frame, whether found by a search or kept from an earlier one
        prev_off = off;
    }
    Float3 proposed{0, 0, 0};
    bool first_in_shot = false;
    const Shot* last_shot = nullptr;
    Float3 prev_off{0, 0, 0};
    ClearanceStats stats;
};

Director::Director(const District& district, const ScoutReport& report, double film_seconds, double preroll) : impl_(std::make_unique<Impl>(district, report, film_seconds, preroll)) {
    for (const StreetProp& p : district.props)
        if (p.type == "tree") impl_->trees.push_back(p);
    impl_->awnings = awning_boxes(district);
}
Director::~Director() = default;

Cam Director::camera(double t, double dt, const World& world) {
    Shot* s = impl_->shot_at(t);
    if (s == nullptr) return {};
    const double u = std::clamp((t - s->t0) / std::max(1e-6, s->t1 - s->t0), 0.0, 1.0);
    Cam c = s->fn(*s, u, dt, world);
    if (s->has_focus && !c.has_subject) {
        c.subject = f3(s->focus);
        c.has_subject = true;
    }
    impl_->proposed = c.eye;
    static const bool no_clearance = std::getenv("SAIGE_ALIVE_NO_CANOPY_CLEAR") != nullptr;   // diagnostics: the film's original behaviour (only the push out of collision boxes)
    if (no_clearance) {
        c.eye = impl_->clear_of_geometry(c.eye);
    } else {
        c.eye = impl_->resolve_eye(c.eye, c.look, c.has_subject ? c.subject : c.look, c.fov_deg, s->tracks_subject, s);
    }
    return c;
}

ClearanceStats Director::clearance_stats() const { return impl_->stats; }
Float3 Director::last_proposed_eye() const { return impl_->proposed; }

bool Director::shot_tracks_subject(double t) const {
    for (const Shot& s : impl_->shots)
        if (t >= s.t0 && t < s.t1) return s.tracks_subject;
    return false;
}

std::string Director::shot_name(double t) const {
    for (const Shot& s : impl_->shots)
        if (t >= s.t0 && t < s.t1) return s.name;
    return impl_->shots.empty() ? "" : impl_->shots.back().name;
}

std::vector<std::pair<double, std::string>> Director::list() const {
    std::vector<std::pair<double, std::string>> out;
    for (const Shot& s : impl_->shots) out.push_back({s.t0, s.name});
    return out;
}

} // namespace saige::alive::rend
