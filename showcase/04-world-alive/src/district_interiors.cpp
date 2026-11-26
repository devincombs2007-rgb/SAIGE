#include "district_detail.hpp"

#include <algorithm>
#include <cmath>

namespace saige::alive {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double floor_z = 0.15;
constexpr double wall_t = 0.3;

struct Dims {
    double hx, hy, h;
};
bool dims_for(const std::string& t, double scale, Dims& out) {
    if (t == "table_round") { out = {0.45, 0.45, 0.75}; return true; }
    if (t == "table_square") { out = {0.4, 0.4, 0.75}; return true; }
    if (t == "chair") { out = {0.23, 0.23, 0.9}; return true; }
    if (t == "stool") { out = {0.2, 0.2, 0.65}; return true; }
    if (t == "counter") { out = {0.5 * scale, 0.35, 1.05}; return true; }
    if (t == "shelf") { out = {0.5 * scale, 0.22, 1.8}; return true; }
    if (t == "display_case") { out = {0.5 * scale, 0.4, 1.1}; return true; }
    if (t == "plant") { out = {0.25, 0.25, 1.2}; return true; }
    if (t == "sofa") { out = {0.9, 0.45, 0.8}; return true; }
    if (t == "lamp") { out = {0.08, 0.08, 4.8}; return true; }
    if (t == "signal_pole") { out = {0.08, 0.08, 4.2}; return true; }
    if (t == "bench") { out = {0.85, 0.28, 0.9}; return true; }
    if (t == "bin") { out = {0.3, 0.3, 0.9}; return true; }
    if (t == "hydrant") { out = {0.18, 0.18, 0.75}; return true; }
    if (t == "tree") { out = {0.22, 0.22, 3.0}; return true; }
    if (t == "bollard") { out = {0.12, 0.12, 0.8}; return true; }
    if (t == "planter") { out = {0.6, 0.6, 0.6}; return true; }
    if (t == "bus_stop") { out = {1.6, 0.45, 2.6}; return true; }
    if (t == "bike_rack") { out = {0.7, 0.2, 0.8}; return true; }
    if (t == "sign_post") { out = {0.06, 0.06, 2.4}; return true; }
    if (t == "cafe_table") { out = {0.4, 0.4, 0.75}; return true; }
    if (t == "cafe_chair") { out = {0.22, 0.22, 0.85}; return true; }
    if (t == "crate") { out = {0.35, 0.35, 0.6}; return true; }
    if (t == "mooring_post") { out = {0.15, 0.15, 0.6}; return true; }
    if (t == "parked_car") { out = {2.2, 0.9, 1.45}; return true; }
    if (t == "kiosk") { out = {1.4, 1.4, 2.6}; return true; }
    (void)scale;
    return false;
}

double plan_overlap(const Aabb& a, const Rect& r) {
    const double w = std::min(static_cast<double>(a.max.x), r.x1) - std::max(static_cast<double>(a.min.x), r.x0);
    const double h = std::min(static_cast<double>(a.max.y), r.y1) - std::max(static_cast<double>(a.min.y), r.y0);
    return w > 0.0 && h > 0.0 ? w * h : 0.0;
}

// Furniture layouts are written by hand per building kind, and the buildings vary in size and door position, so a layout can put a piece through a wall, in the doorway or on the stairs.
// Every layout is pruned against the hard clearance rules (inside the room, off the walls, off the doorway strip, off the staircase and its landings) so that no building can ship with
// an unusable interior. The same rules are checked by validate_district, which reports a piece this pass would have removed.
void prune_furniture(std::vector<StreetProp>& list, const Rect& room, const std::vector<Rect>& walls, const std::vector<Rect>& keep_clear) {
    std::erase_if(list, [&](const StreetProp& p) {
        Aabb b{};
        if (!prop_box(p, b)) return false;
        if (b.min.x < room.x0 - 1e-6 || b.min.y < room.y0 - 1e-6 || b.max.x > room.x1 + 1e-6 || b.max.y > room.y1 + 1e-6) return true;
        for (const Rect& w : walls)
            if (plan_overlap(b, w) > 1e-3) return true;
        for (const Rect& c : keep_clear)
            if (plan_overlap(b, c) > 1e-3) return true;
        return false;
    });
}

StreetProp make(const char* type, double x, double y, double z, double yaw, double scale = 1.0) {
    StreetProp p;
    p.type = type;
    p.x = x;
    p.y = y;
    p.z = z;
    p.yaw = yaw;
    p.scale = scale;
    return p;
}

// A wall of the room, as rectangles on the building's perimeter with a gap for the door on the front face.
void perimeter_walls(const Building& b, double door_w, std::vector<Rect>& out) {
    const Rect& f = b.footprint;
    const double x0 = f.x0, x1 = f.x1, y0 = f.y0, y1 = f.y1;
    const double dxl = b.door_x - 0.5 * door_w, dxr = b.door_x + 0.5 * door_w;
    const double dyl = b.door_y - 0.5 * door_w, dyr = b.door_y + 0.5 * door_w;
    switch (b.front) {
    case Leg::South:
        out.push_back({x0, y0, dxl, y0 + wall_t});
        out.push_back({dxr, y0, x1, y0 + wall_t});
        out.push_back({x0, y1 - wall_t, x1, y1});
        out.push_back({x0, y0 + wall_t, x0 + wall_t, y1 - wall_t});
        out.push_back({x1 - wall_t, y0 + wall_t, x1, y1 - wall_t});
        break;
    case Leg::North:
        out.push_back({x0, y1 - wall_t, dxl, y1});
        out.push_back({dxr, y1 - wall_t, x1, y1});
        out.push_back({x0, y0, x1, y0 + wall_t});
        out.push_back({x0, y0 + wall_t, x0 + wall_t, y1 - wall_t});
        out.push_back({x1 - wall_t, y0 + wall_t, x1, y1 - wall_t});
        break;
    case Leg::East:
        out.push_back({x1 - wall_t, y0, x1, dyl});
        out.push_back({x1 - wall_t, dyr, x1, y1});
        out.push_back({x0, y0, x0 + wall_t, y1});
        out.push_back({x0 + wall_t, y0, x1 - wall_t, y0 + wall_t});
        out.push_back({x0 + wall_t, y1 - wall_t, x1 - wall_t, y1});
        break;
    case Leg::West:
        out.push_back({x0, y0, x0 + wall_t, dyl});
        out.push_back({x0, dyr, x0 + wall_t, y1});
        out.push_back({x1 - wall_t, y0, x1, y1});
        out.push_back({x0 + wall_t, y0, x1 - wall_t, y0 + wall_t});
        out.push_back({x0 + wall_t, y1 - wall_t, x1 - wall_t, y1});
        break;
    }
}

// Local frame of a building: u runs along the facade (left to right as seen from the street), v runs from the facade into the building; returns world coordinates.
struct Frame {
    Leg front;
    Rect f;
    double u0, u1, depth;  // the facade's extent and the building's depth
    [[nodiscard]] std::array<double, 2> world(double u, double v) const {
        switch (front) {
        case Leg::South: return {f.x0 + u, f.y0 + v};
        case Leg::North: return {f.x1 - u, f.y1 - v};  // x runs the other way so that u is left to right for the viewer facing south... mirrored consistently
        case Leg::East: return {f.x1 - v, f.y0 + u};
        case Leg::West: return {f.x0 + v, f.y1 - u};
        }
        return {0, 0};
    }
    // Yaw of a prop whose local +y faces the facade (v towards the street).
    [[nodiscard]] double yaw_facing_street() const {
        switch (front) {
        case Leg::South: return 0.0;
        case Leg::North: return kPi;
        case Leg::East: return kPi * 0.5;
        case Leg::West: return -kPi * 0.5;
        }
        return 0.0;
    }
};

Frame frame_of(const Building& b) {
    Frame fr{b.front, b.footprint, 0, 0, 0};
    const bool horiz = (b.front == Leg::South || b.front == Leg::North);
    fr.u1 = horiz ? b.footprint.w() : b.footprint.h();
    fr.depth = horiz ? b.footprint.h() : b.footprint.w();
    return fr;
}

void add_furniture(const Frame& fr, std::vector<StreetProp>& out, const char* type, double u, double v, double z, double local_yaw, double scale = 1.0) {
    const auto w = fr.world(u, v);
    // Local yaw 0: the prop's front faces the street.
    out.push_back(make(type, w[0], w[1], z, fr.yaw_facing_street() + local_yaw, scale));
}

} // namespace

bool prop_box(const StreetProp& p, Aabb& out) {
    Dims dm{};
    if (!dims_for(p.type, p.scale, dm)) return false;
    const double c = std::abs(std::cos(p.yaw)), s = std::abs(std::sin(p.yaw));
    const double hx = dm.hx * c + dm.hy * s, hy = dm.hx * s + dm.hy * c;
    out = box(p.x - hx, p.y - hy, p.z, p.x + hx, p.y + hy, p.z + dm.h);
    return true;
}

void build_interiors(District& d, Rng& rng) {
    (void)rng;
    // Choose the accessible buildings: for each commercial kind, the building nearest to the middle of the district that is large enough and not facing a side street's end.
    struct Want {
        BuildingKind kind;
        double min_w, min_d;
        int count;
    };
    const Want wants[] = {{BuildingKind::Cafe, 11.0, 12.0, 1}, {BuildingKind::Shop, 9.0, 11.0, 1}, {BuildingKind::Bakery, 9.0, 11.0, 1}, {BuildingKind::Bar, 9.0, 12.0, 1}, {BuildingKind::Pharmacy, 9.0, 11.0, 1}};
    for (const Want& w : wants) {
        int chosen = -1;
        double best = 1e18;
        for (std::size_t i = 0; i < d.buildings.size(); ++i) {
            const Building& b = d.buildings[i];
            if (b.kind != w.kind || b.interior) continue;
            const bool horiz = (b.front == Leg::South || b.front == Leg::North);
            const double wd = horiz ? b.footprint.w() : b.footprint.h();
            const double dp = horiz ? b.footprint.h() : b.footprint.w();
            if (wd < w.min_w || dp < w.min_d) continue;
            // The cafe wants the waterfront; the others the main streets near the middle.
            const double score = std::hypot(b.footprint.cx(), b.footprint.cy() - (w.kind == BuildingKind::Cafe ? 30.0 : 5.0));
            if (score < best) {
                best = score;
                chosen = static_cast<int>(i);
            }
        }
        if (chosen < 0) {
            // None large enough: promote a nearby building's kind so that every kind has one.
            for (std::size_t i = 0; i < d.buildings.size(); ++i) {
                Building& b = d.buildings[i];
                if (b.interior || !(b.kind == BuildingKind::Shop || b.kind == BuildingKind::Offices || b.kind == BuildingKind::Apartments)) continue;
                const bool horiz = (b.front == Leg::South || b.front == Leg::North);
                const double wd = horiz ? b.footprint.w() : b.footprint.h();
                const double dp = horiz ? b.footprint.h() : b.footprint.w();
                if (wd >= w.min_w && dp >= w.min_d && std::hypot(b.footprint.cx(), b.footprint.cy() - 5.0) < best) {
                    best = std::hypot(b.footprint.cx(), b.footprint.cy() - 5.0);
                    chosen = static_cast<int>(i);
                }
            }
            if (chosen >= 0) {
                d.buildings[static_cast<std::size_t>(chosen)].kind = w.kind;
                d.buildings[static_cast<std::size_t>(chosen)].name = kind_name(w.kind);
            }
        }
        if (chosen < 0) continue;
        Building& b = d.buildings[static_cast<std::size_t>(chosen)];
        b.interior = true;
        // The door sits clear of the corners.
        const bool horiz = (b.front == Leg::South || b.front == Leg::North);
        if (horiz) b.door_x = std::clamp(b.door_x, b.footprint.x0 + 2.4, b.footprint.x1 - 2.4);
        else b.door_y = std::clamp(b.door_y, b.footprint.y0 + 2.4, b.footprint.y1 - 2.4);
        if (b.kind == BuildingKind::Cafe) b.floors = std::max(b.floors, 3);
        b.floors = std::max(b.floors, 2);

        Interior in;
        in.building = b.id;
        in.ceiling = b.floor_height - 0.25;
        const Rect& f = b.footprint;
        in.room = {f.x0 + wall_t, f.y0 + wall_t, f.x1 - wall_t, f.y1 - wall_t};
        perimeter_walls(b, 1.9, in.walls);
        const Frame fr = frame_of(b);
        const double W = fr.u1, D = fr.depth;
        // Door points: just inside and just outside along the inward normal.
        {
            const auto outside = fr.world(0.0, 0.0);
            (void)outside;
            const auto dir = leg_dir(b.front);
            in.door_outside = {b.door_x + dir[0] * 2.4, b.door_y + dir[1] * 2.4};
            in.door_inside = {b.door_x - dir[0] * 1.2, b.door_y - dir[1] * 1.2};
        }
        const double fz = floor_z;
        auto F = [&](const char* t, double u, double v, double yaw = 0.0, double scale = 1.0) { add_furniture(fr, in.furniture, t, u, v, fz, yaw, scale); };
        switch (b.kind) {
        case BuildingKind::Cafe: {
            // Counter along the back wall, tables in two rows, plants by the front windows; stairs in the back corner.
            F("counter", W * 0.40, D - 1.6, kPi, 4.2);
            F("display_case", W * 0.40 + 2.8, D - 1.6, kPi, 1.4);
            for (int k = 0; k < 3; ++k) {
                for (int r = 0; r < 2; ++r) {
                    const double u = 2.4 + k * 2.6, v = 3.2 + r * 3.0;
                    if (u > W - 5.5) continue;
                    F("table_round", u, v);
                    F("chair", u - 0.8, v, kPi * 0.5);
                    F("chair", u + 0.8, v, -kPi * 0.5);
                }
            }
            F("plant", 0.9, 1.0);
            F("plant", W - 0.9, 1.0);
            // The stairs run along the right-hand wall from the front towards the back: 12 steps of 0.3 m.
            in.has_stairs = true;
            const double su0 = W - 1.9, su1 = W - 0.7;  // the stair's width across u
            const double sv0 = 2.2, sv1 = sv0 + 3.6;
            const auto a = fr.world(su0, sv0), c = fr.world(su1, sv1);
            in.stair_footprint = {std::min(a[0], c[0]), std::min(a[1], c[1]), std::max(a[0], c[0]), std::max(a[1], c[1])};
            in.stair_bottom = fr.world(0.5 * (su0 + su1), sv0 + 0.15);
            in.stair_top = fr.world(0.5 * (su0 + su1), sv1 + 0.4);
            in.upper_floor_z = fz + 12 * (3.4 / 12.0);
            in.upper_room = in.room;
            for (int k = 0; k < 4; ++k) {
                const double u = 2.2 + k * 2.4, v = 3.2 + (k % 2) * 3.4;
                add_furniture(fr, in.upper_furniture, "table_round", u, v, in.upper_floor_z, 0.0);
                add_furniture(fr, in.upper_furniture, "chair", u - 0.8, v, in.upper_floor_z, kPi * 0.5);
                add_furniture(fr, in.upper_furniture, "chair", u + 0.8, v, in.upper_floor_z, -kPi * 0.5);
            }
            add_furniture(fr, in.upper_furniture, "sofa", 2.0, D - 1.3, in.upper_floor_z, kPi);
            break;
        }
        case BuildingKind::Shop: {
            for (int k = 0; k < 3; ++k) F("shelf", 3.0 + k * 2.2, D * 0.55, kPi * 0.5 * 0 + 0.0, 2.0);
            for (int k = 0; k < 3; ++k) F("shelf", 3.0 + k * 2.2, D * 0.3, 0.0, 2.0);
            F("shelf", W * 0.5, D - 0.6, kPi, W - 3.0);
            {
                const double door_u = b.front == Leg::South ? b.door_x - b.footprint.x0 : b.front == Leg::North ? b.footprint.x1 - b.door_x : b.front == Leg::East ? b.door_y - b.footprint.y0 : b.footprint.y1 - b.door_y;
                F("counter", door_u < 0.5 * W ? W - 3.0 : 3.0, 2.0, 0.0, 2.2);   // on the far side from the door's path
            }
            F("crate", 1.0, 1.3);
            break;
        }
        case BuildingKind::Bakery: {
            {   // beside the door's path, on whichever side has more room
                const double door_u = b.front == Leg::South ? b.door_x - b.footprint.x0 : b.front == Leg::North ? b.footprint.x1 - b.door_x : b.front == Leg::East ? b.door_y - b.footprint.y0 : b.footprint.y1 - b.door_y;
                const double u = door_u < 0.5 * W ? std::min(W - 2.3, door_u + 3.4) : std::max(2.3, door_u - 3.4);
                F("display_case", u, 2.6, 0.0, 3.2);
            }
            F("counter", W * 0.5, D - 1.8, kPi, 4.0);
            F("shelf", W * 0.25, D - 0.6, kPi, 2.4);
            F("table_square", 1.8, 4.5);
            F("chair", 1.8, 5.4, kPi);
            F("chair", 1.8, 3.6, 0.0);
            F("plant", W - 0.9, 1.0);
            break;
        }
        case BuildingKind::Bar: {
            F("counter", W * 0.5, D - 2.0, kPi, 6.0);
            for (int k = 0; k < 5; ++k) F("stool", W * 0.5 - 2.4 + k * 1.2, D - 3.1);
            for (int k = 0; k < 3; ++k) {
                if (2.3 + k * 2.8 + 0.8 + 0.23 > W - wall_t - 0.1) continue;   // the right-hand chair must stay inside the wall
                F("table_round", 2.3 + k * 2.8, 3.0);
                F("chair", 2.3 + k * 2.8 - 0.8, 3.0, kPi * 0.5);
                F("chair", 2.3 + k * 2.8 + 0.8, 3.0, -kPi * 0.5);
            }
            break;
        }
        case BuildingKind::Pharmacy: {
            for (int k = 0; k < 3; ++k) F("shelf", 3.0 + k * 2.4, D * 0.5, 0.0, 2.0);
            F("counter", W * 0.5, D - 1.8, kPi, 5.0);
            F("display_case", 2.0, 2.0, 0.0, 1.6);
            break;
        }
        default: break;
        }
        {   // clearance rules: the doorway strip, the staircase and its landings
            const auto dir = leg_dir(b.front);
            const double x0 = b.door_x, y0 = b.door_y;
            const Rect door_strip = std::abs(dir[0]) > 0.5 ? Rect{std::min(x0, x0 - dir[0] * 2.5), y0 - 0.5, std::max(x0, x0 - dir[0] * 2.5), y0 + 0.5} : Rect{x0 - 0.5, std::min(y0, y0 - dir[1] * 2.5), x0 + 0.5, std::max(y0, y0 - dir[1] * 2.5)};
            std::vector<Rect> clear{door_strip};
            std::vector<Rect> clear_upper;
            if (in.has_stairs) {
                const Rect foot = {in.stair_footprint.x0 - 0.3, in.stair_footprint.y0 - 0.3, in.stair_footprint.x1 + 0.3, in.stair_footprint.y1 + 0.3};
                clear.push_back(foot);
                clear.push_back({in.stair_bottom[0] - 0.8, in.stair_bottom[1] - 0.8, in.stair_bottom[0] + 0.8, in.stair_bottom[1] + 0.8});
                clear_upper.push_back(foot);
                clear_upper.push_back({in.stair_top[0] - 0.8, in.stair_top[1] - 0.8, in.stair_top[0] + 0.8, in.stair_top[1] + 0.8});
            }
            prune_furniture(in.furniture, in.room, in.walls, clear);
            prune_furniture(in.upper_furniture, in.upper_room, in.upper_walls, clear_upper);
        }
        d.interiors.push_back(in);
    }
}

// True when a prop of `type` at (x, y) would not overlap any prop already placed.
bool prop_free(const District& d, const char* type, double x, double y, double yaw) {
    StreetProp cand = make(type, x, y, ground_height(d, x, y), yaw);
    Aabb cb{};
    if (!prop_box(cand, cb)) return true;
    for (const StreetProp& q : d.props) {
        Aabb qb{};
        if (!prop_box(q, qb)) continue;
        if (qb.min.z >= cb.max.z || cb.min.z >= qb.max.z) continue;
        if (plan_overlap(cb, {static_cast<double>(qb.min.x) - 0.15, static_cast<double>(qb.min.y) - 0.15, static_cast<double>(qb.max.x) + 0.15, static_cast<double>(qb.max.y) + 0.15}) > 0.0) return false;
    }
    return true;
}

void build_props(District& d, Rng& rng) {
    auto add = [&](const char* type, double x, double y, double yaw = 0.0, double scale = 1.0) {
        StreetProp p = make(type, x, y, ground_height(d, x, y), yaw, scale);
        d.props.push_back(p);
    };
    // Intersection furniture: signal poles at two opposite corners of each approach, bollards at crosswalk curb ramps, hydrants.
    for (const Intersection& I : d.intersections) {
        for (int sx : {-1, 1}) {
            for (int sy : {-1, 1}) {
                const double cx = I.x + sx * (road_half + 0.6), cy = I.y + sy * (road_half + 0.6);
                if ((sx > 0) == (sy > 0)) add("signal_pole", cx, cy, std::atan2(-sy, -sx));
                else add("bollard", cx, cy);
            }
        }
        if (I.id % 2 == 1) add("hydrant", I.x + row_half - 1.0, I.y - row_half + 1.0);
    }
    // Street lamps and trees along the sidewalks, every 22 m, clear of the intersections and driveways; benches and bins in between.
    const auto along = [&](bool horizontal, double fixed, double a, double b, double step, double side) {
        int n = 0;
        for (double t = a; t < b; t += step, ++n) {
            const double x = horizontal ? t : fixed, y = horizontal ? fixed : t;
            // Skip positions within an intersection's corridor.
            bool near_x = false;
            for (const Intersection& I : d.intersections) {
                if (std::abs(x - I.x) < row_half + 1.0 && std::abs(y - I.y) < row_half + 1.0) near_x = true;
            }
            if (near_x || over_water(d, x, y)) continue;
            switch (n % 4) {
            case 0: add("lamp", x, y, horizontal ? (side > 0 ? -kPi * 0.5 : kPi * 0.5) : (side > 0 ? kPi : 0.0)); break;
            case 1: add("tree", x, y, rng.range(0, 6.28), rng.range(0.9, 1.15)); break;
            case 2: add(rng.pick(2) ? "bench" : "bin", x, y, horizontal ? (side > 0 ? kPi : 0.0) : (side > 0 ? kPi * 0.5 : -kPi * 0.5)); break;
            default: add("tree", x, y, rng.range(0, 6.28), rng.range(0.9, 1.15)); break;
            }
        }
    };
    for (const double sy : {-60.0, 0.0, 60.0}) {
        // The furniture line is 1 m kerbside of the sidewalk's walking line (where the pedestrian nodes are). Benches sit parallel to the kerb: a bench across the line (1.7 m) reached the asphalt.
        along(true, sy + corner_offset - 1.0, -150.0, 150.0, 11.0, 1.0);
        along(true, sy - corner_offset + 1.0, -150.0, 150.0, 11.0, -1.0);
    }
    for (const double ax : {-90.0, 0.0, 90.0}) {
        along(false, ax + corner_offset - 1.0, -100.0, 60.0, 11.0, 1.0);
        along(false, ax - corner_offset + 1.0, -100.0, 60.0, 11.0, -1.0);
    }
    // Promenade: benches facing the water, lamps, bins, mooring posts along the quay, a kiosk.
    for (double x = -120.0; x < 125.0; x += 15.0) {
        add("lamp", x, d.quay_y - 1.2, 0.0);
        if (std::fmod(std::abs(x), 30.0) < 1.0) add("bench", x + 4.0, d.quay_y - 2.5, kPi * 0.5);
        else add("mooring_post", x + 7.0, d.quay_y - 0.5, 0.0);
    }
    add("kiosk", -20.0, 72.5, 0.0);
    add("bin", -14.0, 72.0, 0.0);
    add("bin", 25.0, 72.0, 0.0);
    // The park: trees, benches, a fence of breakable rails along the road.
    for (const Rect& p : d.park) {
        for (double x = p.x0 + 8; x < p.x1 - 8; x += 16.0) add("bench", x, p.cy(), 0.0);
        for (int i = 0; i < 18; ++i) {
            // A tree needs room: not on a bench or another prop (a few tries; the park is large).
            for (int attempt = 0; attempt < 24; ++attempt) {
                const double x = rng.range(p.x0 + 4, p.x1 - 4), y = rng.range(p.y0 + 4, p.y1 - 4);
                if (!prop_free(d, "tree", x, y, 0.0)) continue;
                add("tree", x, y, rng.range(0, 6.28), rng.range(0.9, 1.3));
                break;
            }
        }
    }
    // Pier furniture.
    for (const Pier& pr : d.piers) {
        for (double y = pr.deck.y0 + 3.0; y < pr.deck.y1 - 2.0; y += 6.0) {
            add("mooring_post", pr.deck.x0 + 0.3, y);
            add("mooring_post", pr.deck.x1 - 0.3, y);
        }
    }
    // Cafe tables and chairs on the sidewalk in front of each cafe and bakery.
    for (const Building& b : d.buildings) {
        if (b.kind != BuildingKind::Cafe && b.kind != BuildingKind::Bakery && b.kind != BuildingKind::Bar) continue;
        const auto dir = leg_dir(b.front);
        for (int k = -1; k <= 1; k += 2) {
            const double px = -dir[1], py = dir[0];
            // The sidewalk is 3.5 m deep from the facade (row_half - road_half). The tables sit against the facade side of it; the street furniture line (lamps, trees, benches) is further out, and neither is
            // in the parking bay beyond the kerb.
            const double x = b.door_x + dir[0] * 0.8 + px * 2.6 * k, y = b.door_y + dir[1] * 0.8 + py * 2.6 * k;
            if (over_water(d, x, y)) continue;
            add("cafe_table", x, y, 0.0);
            add("cafe_chair", x + px * 0.7, y + py * 0.7, std::atan2(-py, -px));
            add("cafe_chair", x - px * 0.7, y - py * 0.7, std::atan2(py, px));
        }
    }
    // A bus stop on Market Street, bike racks near the shops, crates behind the bar.
    // Each is placed where it is free: slid along the street away from whatever (a cafe's tables, a lamp) is already there.
    const auto add_free = [&](const char* type, double x, double y, double yaw) {
        for (const double dx : {0.0, 3.0, -3.0, 6.0, -6.0, 9.0, -9.0, 12.0, -12.0}) {
            if (!prop_free(d, type, x + dx, y, yaw)) continue;
            add(type, x + dx, y, yaw);
            return;
        }
    };
    add_free("bus_stop", -45.0, 0.0 + corner_offset + 1.0, 0.0);
    add_free("bike_rack", 30.0, -corner_offset - 1.0, 0.0);
    add_free("bike_rack", -30.0, corner_offset + 1.0, 0.0);
}

} // namespace saige::alive
