#include "director.hpp"

#include "district_validate.hpp"
#include "vegetation_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

struct Shot {
    double t0 = 0, t1 = 0;
    std::string name;
    std::function<Cam(Shot&, double u, double dt, const World&)> fn;
    // per-shot state
    std::uint64_t subject = 0;
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
            return Cam{f3(eye), f3(look), 52.0f};
        };
        return s;
    }

    // Chasing a moving car (or sitting at its bumper or windscreen: `mode` 0 chase, 1 bumper, 2 driver).
    static Shot follow_car(double t0, double t1, const char* name, V near, int mode, double back = 7.5, double height = 2.3, double side = 0.8) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
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
            return Cam{f3(eye), f3(look), 55.0f};
        };
        return s;
    }

    static Shot follow_vehicle_id(double t0, double t1, const char* name, std::uint32_t id, double back, double height, double side, V fallback) {
        Shot s;
        s.t0 = t0;
        s.t1 = t1;
        s.name = name;
        s.fn = [=](Shot& sh, double, double dt, const World& w) {
            const std::vector<VehicleView> vs = w.vehicles();
            const VehicleView* v = find_vehicle(vs, id);
            if (v == nullptr) return Cam{f3(fallback + V{0, 0, 3}), f3(fallback), 55.0f};
            const V pos{v->x, v->y, v->z};
            const V fwd{std::cos(v->yaw), std::sin(v->yaw), 0};
            const V left{-fwd.y, fwd.x, 0};
            const V eye = sh.eye.update(pos - fwd * back + left * side + V{0, 0, height}, 0.5, dt);
            const V look = sh.look.update(pos + fwd * 8.0 + V{0, 0, 0.6}, 0.3, dt);
            return Cam{f3(eye), f3(look), 52.0f};
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
        const bool has_accident = rep.accident_t > 0.0;
        const double ta = has_accident ? rep.accident_t - preroll : -1.0;   // film time of the accident
        // Intro: ordinary life, scaled to end 9 s before the accident (or to take 60% of the film without one).
        struct Base {
            const char* name;
            double dur;
        };
        const double intro_end = has_accident ? std::max(40.0, ta - 9.0) : T * 0.6;
        std::vector<Base> intro = {{"harbor", 13}, {"cafe exterior", 11}, {"crossing", 13}, {"chase", 13}, {"follow", 11}, {"cafe interior", 9}, {"driver", 8}};
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
        {
            const auto [a, b] = next(intro[1]);
            shots.push_back(dolly(a, b, "cafe terrace", door + out * 12.0 - along * 14.0 + V{0, 0, 1.75}, door + out * 11.5 + along * 9.0 + V{0, 0, 1.8}, door + out * 1.2 + V{0, 0, 1.6}, door + out * 1.2 + along * 1.5 + V{0, 0, 1.6}, 52, 46));
        }
        {
            const auto [a, b] = next(intro[2]);
            shots.push_back(dolly(a, b, "crossing at the main intersection", {12.5, -14.5, 3.3}, {8.8, -10.8, 2.5}, {0, -1.5, 1.6}, {0, -0.5, 1.4}, 56, 46));
        }
        {
            const auto [a, b] = next(intro[3]);
            shots.push_back(follow_car(a, b, "chase cam", {0, -30, 0}, 0));
        }
        {
            const auto [a, b] = next(intro[4]);
            shots.push_back(follow_ped(a, b, "follow a resident", {-90, 30, 0}, 3.4, 1.9, 0.4));
        }
        {
            const auto [a, b] = next(intro[5]);
            shots.push_back(dolly(a, b, "inside the cafe", door - out * 1.6 - along * 6.0 + V{0, 0, 1.75}, door - out * 1.8 - along * 4.0 + V{0, 0, 1.7}, door - out * 8.0 + along * 3.0 + V{0, 0, 1.2}, door - out * 8.0 + along * 2.0 + V{0, 0, 1.3}, 78, 72));
        }
        {
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
            const V side = on_avenue ? V{1.0, 0.0, 0.0} : V{0.0, 1.0, 0.0};   // unit, horizontal
            const auto cam_at = [&](double dist, double height) { return at + side * dist + V{0, 0, height}; };
            const V perp{-side.y, side.x, 0.0};
            shots.push_back(dolly(ta + 0.9, ta + 9.0, "the wreck", cam_at(8.0, 1.7) + perp * 7.0, cam_at(5.5, 1.5) + perp * 5.0, at + V{0, 0, 0.7}, at + V{0, 0, 0.9}, 50, 42));
            shots.push_back(dolly(ta + 9.0, ta + 22.0, "the street reacts", cam_at(-12.0, 1.8) - perp * 9.0, cam_at(-9.0, 1.9) - perp * 6.0, at + V{0, 0, 1.0}, at + V{0, 0, 1.2}, 58, 50));
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
                shots.push_back(hold(resp0, on - 3.0, "street", cam_at(-16.0, 2.5), at, 55));
            }
            shots.push_back(dolly(on - 3.0, acc_end, "at the scene", cam_at(12.0, 1.8) + V{0, 0, 0} + (on_avenue ? V{0, -4, 0} : V{-4, 0, 0}), cam_at(8.0, 2.4) + (on_avenue ? V{0, 4, 0} : V{4, 0, 0}), at + V{0, 0, 1.2}, at + V{0, 0, 1.4}, 55, 48));
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

    // Foliage: the collision set only has a tree's trunk, but the leaves are what a camera at human height ends up inside. The eye is pushed out of every canopy sphere (plus a margin),
    // and when the first metres of the line of sight pass through leaves the eye backs away from the nearest canopy until the view is clear.
    Float3 clear_of_canopy(Float3 eye, Float3 look) const {
        constexpr float margin = 0.5f, view_len = 6.0f;
        for (int pass = 0; pass < 12; ++pass) {
            bool moved = false;
            Float3 dir{look.x - eye.x, look.y - eye.y, look.z - eye.z};
            const float dl = std::max(1e-6f, std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z));
            dir = {dir.x / dl, dir.y / dl, dir.z / dl};
            for (const StreetProp& t : trees) {
                if (std::hypot(static_cast<float>(t.x) - eye.x, static_cast<float>(t.y) - eye.y) > 14.0f) continue;
                const auto blobs = tree_blobs(static_cast<float>(t.x), static_cast<float>(t.y), static_cast<float>(t.z), static_cast<float>(t.yaw), static_cast<float>(t.scale), tree_seed(t));
                for (const CanopyBlob& b : blobs) {
                    // The eye inside (or within the margin of) the blob: out along the line from the blob's centre.
                    Float3 off{eye.x - b.x, eye.y - b.y, eye.z - b.z};
                    float dist = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
                    if (dist < b.r + margin) {
                        if (dist < 1e-3f) { off = {1, 0, 0}; dist = 1.0f; }
                        const float k = (b.r + margin) / dist;
                        eye = {b.x + off.x * k, b.y + off.y * k, b.z + off.z * k};
                        moved = true;
                        continue;
                    }
                    // The line of sight through the blob within view_len: back the eye away from the blob's centre (horizontally) by the penetration.
                    const Float3 oc{b.x - eye.x, b.y - eye.y, b.z - eye.z};
                    const float tc = oc.x * dir.x + oc.y * dir.y + oc.z * dir.z;
                    if (tc <= 0.0f || tc - b.r > view_len) continue;
                    const Float3 closest{eye.x + dir.x * tc, eye.y + dir.y * tc, eye.z + dir.z * tc};
                    const float miss = std::sqrt((closest.x - b.x) * (closest.x - b.x) + (closest.y - b.y) * (closest.y - b.y) + (closest.z - b.z) * (closest.z - b.z));
                    if (miss < b.r) {
                        const float hx = eye.x - b.x, hy = eye.y - b.y, hl = std::max(1e-3f, std::hypot(hx, hy));
                        const float push = b.r - miss + 0.3f;
                        eye.x += hx / hl * push;
                        eye.y += hy / hl * push;
                        moved = true;
                    }
                }
            }
            if (!moved) break;
        }
        return eye;
    }
    std::vector<StreetProp> trees;
    std::vector<Box3> awnings;

    // Awnings are not in the collision set. When awnings (or leaves) cover part of the frame the eye steps toward the street side of the awning (away from its building) in 0.3 m steps, never
    // more than 3 m in all, so the shot keeps its subject; a shot that is still obstructed after that is reported by the film's camera check instead of being dragged away.
    Float3 clear_of_awnings(Float3 eye, Float3 look, float fov_deg) const {
        const Float3 start = eye;
        for (int step = 0; step < 10; ++step) {
            if (!view_obstructed(trees, awnings, {eye.x, eye.y, eye.z}, {look.x, look.y, look.z}, fov_deg)) break;
            // Which is nearer, a canopy or an awning? A canopy is pushed away from its nearest sphere's centre (horizontally), an awning toward the street side.
            {
                float tbest = 1e9f, tx = 0, ty = 0;
                for (const StreetProp& t : trees) {
                    if (std::hypot(static_cast<float>(t.x) - eye.x, static_cast<float>(t.y) - eye.y) > 12.0f) continue;
                    for (const CanopyBlob& b : tree_blobs(static_cast<float>(t.x), static_cast<float>(t.y), static_cast<float>(t.z), static_cast<float>(t.yaw), static_cast<float>(t.scale), tree_seed(t))) {
                        const float c = std::sqrt((eye.x - b.x) * (eye.x - b.x) + (eye.y - b.y) * (eye.y - b.y) + (eye.z - b.z) * (eye.z - b.z)) - b.r;
                        if (c < tbest) { tbest = c; tx = b.x; ty = b.y; }
                    }
                }
                float abest = 1e9f;
                for (const Box3& a : awnings) {
                    const float cx = std::clamp(eye.x, a.x0, a.x1), cy = std::clamp(eye.y, a.y0, a.y1);
                    abest = std::min(abest, std::hypot(cx - eye.x, cy - eye.y));
                }
                if (tbest < 4.0f && tbest <= abest) {
                    const float hx = eye.x - tx, hy = eye.y - ty, hl = std::max(1e-3f, std::hypot(hx, hy));
                    eye.x += hx / hl * 0.3f;
                    eye.y += hy / hl * 0.3f;
                    if (std::hypot(eye.x - start.x, eye.y - start.y) > 3.0f) break;
                    continue;
                }
            }
            const Box3* best = nullptr;
            float bd = 1e9f;
            for (const Box3& a : awnings) {
                const float cx = std::clamp(eye.x, a.x0, a.x1), cy = std::clamp(eye.y, a.y0, a.y1), d2 = (cx - eye.x) * (cx - eye.x) + (cy - eye.y) * (cy - eye.y);
                if (d2 < bd) { bd = d2; best = &a; }
            }
            if (best == nullptr || bd > 36.0f) break;
            // Outward = away from the building the awning belongs to (the nearest footprint centre), along the awning's thin axis.
            const float cx = 0.5f * (best->x0 + best->x1), cy = 0.5f * (best->y0 + best->y1);
            float bx = 0, by = 0, bdist = 1e9f;
            for (const Building& b : d.buildings) {
                const double dd = std::hypot(b.footprint.cx() - static_cast<double>(cx), b.footprint.cy() - static_cast<double>(cy));
                if (dd < static_cast<double>(bdist)) { bdist = static_cast<float>(dd); bx = static_cast<float>(b.footprint.cx()); by = static_cast<float>(b.footprint.cy()); }
            }
            const bool thin_x = (best->x1 - best->x0) < (best->y1 - best->y0);
            if (thin_x) eye.x += (cx >= bx ? 1.0f : -1.0f) * 0.3f;
            else eye.y += (cy >= by ? 1.0f : -1.0f) * 0.3f;
            if (std::hypot(eye.x - start.x, eye.y - start.y) > 3.0f) break;
        }
        return eye;
    }
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
    c.eye = impl_->clear_of_geometry(c.eye);
    static const bool no_canopy_clear = std::getenv("SAIGE_ALIVE_NO_CANOPY_CLEAR") != nullptr;   // diagnostics: the film's original behaviour
    if (!no_canopy_clear) {
        // Foliage and awnings both: a move for one can bring the other, so alternate a few times.
        for (int i = 0; i < 2; ++i) c.eye = impl_->clear_of_geometry(impl_->clear_of_awnings(impl_->clear_of_canopy(c.eye, c.look), c.look, c.fov_deg));
    }
    return c;
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
