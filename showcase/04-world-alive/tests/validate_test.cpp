// Showcase 04: district validation tests. Two kinds: (1) the generated districts are clean (no finding of error severity on 16 seeds), and (2) the validator is not vacuous: each defect class is
// injected into a clean district and must be reported, with the code and ids an editor would act on. Plus the geometry rules the renderer depends on (exposed spans of a wall, the
// interior wall face mask, the shared tree shape). A passing run of this file does not say the film looks right; the visual regression script does (tools/visual_regression.py).
#include "district_validate.hpp"
#include "traffic_driver.hpp"

#include "district_detail.hpp"
#include "vegetation_shape.hpp"

#include <array>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace saige::alive;

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                     \
            std::printf(__VA_ARGS__);                                                                         \
            std::printf("\n");                                                                                \
            ++failures;                                                                                       \
        }                                                                                                     \
    } while (0)

const District& base_district() {
    static const District d = build_district(8);
    return d;
}

std::size_t blocking_of(const ValidationReport& r, const char* code) {
    std::size_t n = 0;
    for (const Finding& f : r.findings)
        if (f.code == code && f.severity >= Severity::Error) ++n;
    return n;
}

const Building* find_kind(const District& d, BuildingKind k, bool interior) {
    for (const Building& b : d.buildings)
        if (b.kind == k && b.interior == interior) return &b;
    return nullptr;
}

// ---- 1. generated districts are clean -----------------------------------------------------------------------------------------------------------------------------------------------------
void test_generated_districts_are_clean() {
    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        const District d = build_district(seed);
        const ValidationReport r = validate_district(d);
        for (const Finding& f : r.findings)
            if (f.severity >= Severity::Error) std::printf("  seed %u: [%s] %s at (%.2f, %.2f): %s\n", seed, severity_name(f.severity), f.code.c_str(), f.x, f.y, f.message.c_str());
        CHECK(!r.has_blocking(), "seed %u has %zu blocking finding(s)", seed, r.count_at_least(Severity::Error));
    }
}

// ---- 2. the validator reports each injected defect ----------------------------------------------------------------------------------------------------------------------------------------
void test_building_overlap_and_road() {
    District d = base_district();
    Building copy = d.buildings[10];
    copy.id = 9000;
    copy.footprint = {d.buildings[10].footprint.x0 + 1.0, d.buildings[10].footprint.y0 + 1.0, d.buildings[10].footprint.x1 + 1.0, d.buildings[10].footprint.y1 + 1.0};
    d.buildings.push_back(copy);
    const ValidationReport r = validate_district(d);
    CHECK(blocking_of(r, "BLD_OVERLAP") >= 1, "an overlapping building must be reported");

    District e = base_district();
    Building on_road = e.buildings[0];
    on_road.id = 9001;
    on_road.footprint = {e.roadway[0].x0 + 1.0, e.roadway[0].y0 + 5.0, e.roadway[0].x0 + 6.0, e.roadway[0].y0 + 12.0};
    e.buildings.push_back(on_road);
    CHECK(blocking_of(validate_district(e), "BLD_ON_ROAD") >= 1, "a building on the roadway must be reported");
}

void test_coplanar_walls() {
    District d = base_district();
    // A second building with the same south face as an existing one and a different depth: the two south faces are one plane and overlap.
    Building twin = d.buildings[3];
    twin.id = 9002;
    twin.footprint.y1 = twin.footprint.y0 + 6.0;
    d.buildings.push_back(twin);
    const ValidationReport r = validate_district(d);
    CHECK(blocking_of(r, "WALL_COPLANAR") >= 1, "two buildings with a shared south plane must be reported as coplanar");
    bool ids_ok = false;
    for (const Finding& f : r.findings)
        if (f.code == "WALL_COPLANAR" && f.ids.size() == 2 && (f.ids[0] == 9002 || f.ids[1] == 9002)) ids_ok = true;
    CHECK(ids_ok, "the finding names the buildings involved");
}

void test_props_on_road_and_in_parking() {
    District d = base_district();
    StreetProp p;
    p.type = "cafe_table";
    p.x = d.roadway[3].cx();
    p.y = d.roadway[3].cy();
    p.z = 0.0;
    d.props.push_back(p);
    CHECK(blocking_of(validate_district(d), "PROP_ON_ROAD") >= 1, "a table in the roadway must be reported");

    District e = base_district();
    CHECK(!e.parking.empty(), "the district has parking bays");
    StreetProp q;
    q.type = "cafe_chair";
    q.x = e.parking[0].x;
    q.y = e.parking[0].y;
    q.z = 0.0;
    e.props.push_back(q);
    CHECK(blocking_of(validate_district(e), "PROP_IN_PARKING") >= 1, "a chair in a parking bay must be reported");
}

void test_hazard_lamps() {
    // The lamps sit at the corners of the VISIBLE body (a BodyBox), one per corner, inside it by the stated insets, at the stated height; and in world space, across vehicle poses, they stay
    // attached: they round-trip through the pose, keep their mutual distances (no scale, shear or mirror) and stay inside the oriented body box. (Delivered: 16 lamps, 12 of them 2 m from their
    // corner; after the first fix the corners came from the physics box, which is not the visible body, and a rendered frame showed a lamp hanging in the air.)
    static_assert(kHazardLampsPerModel * kHazardCorners == 4, "four hazard lamps in all");
    std::vector<BodyBox> boxes;
    for (const VehicleKind k : {VehicleKind::Sedan, VehicleKind::Hatchback, VehicleKind::Van, VehicleKind::Taxi, VehicleKind::Ambulance, VehicleKind::FireEngine}) {
        const saige::VehicleConfig cfg = vehicle_config_for(k);
        boxes.push_back({{-cfg.half_extents.x, -cfg.half_extents.y, -cfg.half_extents.z}, {cfg.half_extents.x, cfg.half_extents.y, cfg.half_extents.z}});
    }
    boxes.push_back({{-2.13f, -0.93f, -0.52f}, {2.07f, 0.95f, 0.71f}});   // a body that is off-centre in x, like a real mesh
    boxes.push_back({{-1.0f, -0.5f, 0.0f}, {1.0f, 0.5f, 0.4f}});
    for (const BodyBox& b : boxes) {
        const auto offs = hazard_lamp_offsets(b);
        int fl = 0, fr = 0, rl = 0, rr = 0;
        for (const auto& o : offs) {
            const bool front = o[0] > 0.5f * (b.lo[0] + b.hi[0]), left = o[1] > 0.5f * (b.lo[1] + b.hi[1]);
            (front ? (left ? fl : fr) : (left ? rl : rr))++;
            CHECK(std::fabs((front ? b.hi[0] - o[0] : o[0] - b.lo[0]) - kHazardInsetX) < 1e-5f, "a lamp is the stated inset inside the end of the body");
            CHECK(std::fabs((left ? b.hi[1] - o[1] : o[1] - b.lo[1]) - kHazardInsetY) < 1e-5f, "a lamp is the stated inset inside the side of the body");
            CHECK(std::fabs(o[2] - (b.lo[2] + kHazardHeightFrac * (b.hi[2] - b.lo[2]))) < 1e-5f, "a lamp is at the stated height");
            for (int axis = 0; axis < 3; ++axis) CHECK(o[static_cast<std::size_t>(axis)] > b.lo[static_cast<std::size_t>(axis)] && o[static_cast<std::size_t>(axis)] < b.hi[static_cast<std::size_t>(axis)], "a lamp is inside the body box");
        }
        CHECK(fl == 1 && fr == 1 && rl == 1 && rr == 1, "one lamp per corner");

        // World space across poses.
        for (int i = 0; i < 24; ++i) {
            const double yaw = 0.27 * i, pitch = 0.05 * (i % 5) - 0.1, roll = 0.04 * (i % 7) - 0.12;
            const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2), cr = std::cos(roll / 2), sr = std::sin(roll / 2);
            const std::array<float, 4> q{static_cast<float>(sr * cp * cy - cr * sp * sy), static_cast<float>(cr * sp * cy + sr * cp * sy), static_cast<float>(cr * cp * sy - sr * sp * cy), static_cast<float>(cr * cp * cy + sr * sp * sy)};
            const std::array<double, 3> pos{37.0 - 3.1 * i, -51.0 + 2.3 * i, 0.4 + 0.01 * i};
            std::array<std::array<double, 3>, kHazardCorners> w{};
            for (std::size_t k = 0; k < offs.size(); ++k) {
                w[k] = body_point_to_world(pos, q, offs[k]);
                const auto back = world_point_to_body(pos, q, w[k]);
                for (std::size_t a = 0; a < 3; ++a) CHECK(std::fabs(back[a] - static_cast<double>(offs[k][a])) < 2e-5, "a lamp round-trips through the vehicle's pose");
                // inside the oriented body box, 1 cm tolerance
                for (std::size_t a = 0; a < 3; ++a) CHECK(back[a] > static_cast<double>(b.lo[a]) - 0.01 && back[a] < static_cast<double>(b.hi[a]) + 0.01, "a lamp stays inside the oriented body box");
            }
            for (std::size_t k = 0; k < offs.size(); ++k)
                for (std::size_t m = k + 1; m < offs.size(); ++m) {
                    double dw = 0, db = 0;
                    for (std::size_t a = 0; a < 3; ++a) { dw += (w[k][a] - w[m][a]) * (w[k][a] - w[m][a]); db += static_cast<double>((offs[k][a] - offs[m][a]) * (offs[k][a] - offs[m][a])); }
                    CHECK(std::fabs(std::sqrt(dw) - std::sqrt(db)) < 3e-5, "the lamps keep their mutual distances (rigid transform)");
                }
            // a point at the body-frame origin lands on the vehicle position (the transform has no offset of its own)
            const auto o = body_point_to_world(pos, q, {0, 0, 0});
            for (std::size_t a = 0; a < 3; ++a) CHECK(std::fabs(o[a] - pos[a]) < 1e-9, "the body origin is the vehicle position");
        }
    }
}

void test_hazard_surface_anchors() {
    // A rounded body (a superellipse |x/a|^4 + |y/b|^4 = 1 in plan, stacked in z): the bounding-box corner is NOT on it (the second fix put lamps there and one hung in the air in a rendered
    // frame); the anchors must be vertices of the mesh, at the right corner, in the middle height band, and the farthest toward their corner.
    const float a = 2.1f, b = 0.95f;
    std::vector<float> v;
    for (int zi = 0; zi <= 8; ++zi)
        for (int k = 0; k < 720; ++k) {
            const float th = 6.2831853f * static_cast<float>(k) / 720.0f, c = std::cos(th), s = std::sin(th);
            const float x = a * std::copysign(std::pow(std::fabs(c), 0.5f), c), y = b * std::copysign(std::pow(std::fabs(s), 0.5f), s);   // |x/a|^4 + |y/b|^4 = 1
            v.insert(v.end(), {x, y, 0.1f * static_cast<float>(zi)});
        }
    const auto anchors = hazard_surface_anchors(v);
    std::size_t idx = 0;
    for (const float sx : {-1.0f, 1.0f})
        for (const float sy : {-1.0f, 1.0f}) {
            const auto& an = anchors[idx++];
            CHECK(an[0] * sx > 0.0f && an[1] * sy > 0.0f, "an anchor is at its own corner (end and side)");
            CHECK(an[2] >= 0.2f - 1e-5f && an[2] <= 0.6f + 1e-5f, "an anchor is in the middle height band");
            const float surf = std::pow(std::fabs(an[0] / a), 4.0f) + std::pow(std::fabs(an[1] / b), 4.0f);
            CHECK(std::fabs(surf - 1.0f) < 1e-3f, "an anchor lies on the body's surface");
            bool is_vertex = false;
            for (std::size_t i = 0; i + 2 < v.size(); i += 3) is_vertex = is_vertex || (v[i] == an[0] && v[i + 1] == an[1] && v[i + 2] == an[2]);
            CHECK(is_vertex, "an anchor is a vertex of the mesh");
            // the bounding-box corner (inset like the box rule) is off the surface by far more than a lamp's size
            const float cx = sx * (a - kHazardInsetX), cyy = sy * (b - kHazardInsetY);
            const float box_surf = std::pow(std::fabs(cx / a), 4.0f) + std::pow(std::fabs(cyy / b), 4.0f);
            CHECK(std::fabs(box_surf - 1.0f) > 0.05f, "(test power) a bounding-box corner is not on this body");
            // brute force: no vertex in the band scores higher
            float best = -1e30f;
            for (std::size_t i = 0; i + 2 < v.size(); i += 3) if (v[i + 2] >= 0.2f - 1e-5f && v[i + 2] <= 0.6f + 1e-5f) best = std::max(best, sx * v[i] + 0.6f * sy * v[i + 1]);
            CHECK(std::fabs((sx * an[0] + 0.6f * sy * an[1]) - best) < 1e-5f, "no vertex of the band lies farther toward the corner");
        }
    CHECK(hazard_surface_anchors({})[0][0] == 0.0f, "an empty mesh yields zero anchors, not garbage");
}

void test_seat_facing() {
    // A table with chairs: a chair must face the table within 30 degrees. The chair model's front is (cos yaw, sin yaw) (tools/check_prop_orientation.py checks the real mesh).
    const auto seat_findings = [](double chair_yaw_deg, double dist = 0.8) {
        District d = base_district();
        // Stand the set on open sidewalk so only the seating rule can fire.
        d.props.clear();
        StreetProp t; t.type = "cafe_table"; t.x = d.buildings[0].footprint.x1 + 20.0; t.y = d.buildings[0].footprint.y1 + 20.0; d.props.push_back(t);
        StreetProp c; c.type = "cafe_chair"; c.x = t.x - dist; c.y = t.y; c.yaw = chair_yaw_deg * 3.14159265358979323846 / 180.0; d.props.push_back(c);   // west of the table: facing it is +X, yaw 0
        return blocking_of(validate_district(d), "SEAT_FACING");
    };
    CHECK(seat_findings(0.0) == 0, "a chair facing its table must not be reported");
    CHECK(seat_findings(25.0) == 0, "25 degrees off is within the tolerance");
    CHECK(seat_findings(-25.0) == 0, "-25 degrees off is within the tolerance");
    CHECK(seat_findings(35.0) == 1, "35 degrees off must be reported");
    CHECK(seat_findings(90.0) == 1, "a chair sideways to its table (the delivered interiors) must be reported");
    CHECK(seat_findings(180.0) == 1, "a chair with its back to the table must be reported");
    CHECK(seat_findings(90.0, 2.5) == 0, "a chair 2.5 m from the table belongs to no table and is not judged");
    // Interiors and upper floors are judged the same way (a mutant that skipped them survived the generated-district check once the generator was fixed).
    for (const bool upper : {false, true}) {
        District d = base_district();
        d.props.clear();
        Interior in;
        in.building = d.buildings[0].id;
        StreetProp t; t.type = "table_round"; t.x = 10.0; t.y = 10.0;
        StreetProp ok; ok.type = "chair"; ok.x = 9.2; ok.y = 10.0; ok.yaw = 0.0;
        StreetProp bad; bad.type = "chair"; bad.x = 10.8; bad.y = 10.0; bad.yaw = 3.14159265358979323846 * 0.5;
        auto& list = upper ? in.upper_furniture : in.furniture;
        list = {t, ok, bad};
        d.interiors.push_back(in);
        CHECK(blocking_of(validate_district(d), "SEAT_FACING") == 1, "exactly the sideways chair of an interior (or upper floor) must be reported");
    }
    // The generated districts: every chair beside a table faces it, on the street, on the ground floor and upstairs.
    // World-space check across the four building fronts: the interiors are built in a frame that is rotated and mirrored per front, so a yaw that is right for one front can be wrong for the others.
    std::array<std::size_t, 4> chairs_by_front{};
    for (const std::uint32_t seed : {8u, 206u, 3u, 4u, 5u, 34u}) {
        const District d = build_district(seed);
        CHECK(blocking_of(validate_district(d), "SEAT_FACING") == 0, "generated chairs must face their tables");
        std::size_t chairs = 0;
        for (const Interior& in : d.interiors) {
            std::size_t here = 0;
            for (const StreetProp& p : in.furniture) here += (p.type == "chair") ? 1u : 0u;
            for (const StreetProp& p : in.upper_furniture) here += (p.type == "chair") ? 1u : 0u;
            chairs += here;
            for (const Building& b : d.buildings)
                if (b.id == in.building) chairs_by_front[static_cast<std::size_t>(b.front)] += here;
        }
        CHECK(chairs >= 10, "the districts have interior chairs (the test would pass vacuously otherwise)");
    }
    for (std::size_t f = 0; f < chairs_by_front.size(); ++f) {
        std::printf("  generated interior chairs on buildings whose front is leg %zu: %zu\n", f, chairs_by_front[f]);
    }
    // The generated districts only have seated interiors on two of the four fronts (legs 0 and 2), so the frames of the other two were never exercised by data. Drive the interior generator on every
    // front: all buildings of a district turned to each leg in turn, the interiors rebuilt, and every chair beside a table must face it, in world space, with chairs present on that front.
    for (const Leg leg : {Leg::South, Leg::East, Leg::North, Leg::West}) {
        District d = build_district(8);
        for (Building& b : d.buildings) b.front = leg;
        d.interiors.clear();
        Rng rng(1234);
        build_interiors(d, rng);
        std::size_t chairs = 0;
        for (const Interior& in : d.interiors)
            for (const StreetProp& p : in.furniture) chairs += (p.type == "chair") ? 1u : 0u;
        std::printf("  rebuilt interiors with every front = leg %d: %zu chairs\n", static_cast<int>(leg), chairs);
        CHECK(chairs >= 6, "the rebuilt interiors have chairs on this front (the check would pass vacuously otherwise)");
        CHECK(blocking_of(validate_district(d), "SEAT_FACING") == 0, "chairs must face their tables on every building front");
    }
}

void test_prop_in_building() {
    District d = base_district();
    const Building& b = d.buildings[20];
    StreetProp p;
    p.type = "bench";
    p.x = b.footprint.cx();
    p.y = b.footprint.cy();
    d.props.push_back(p);
    CHECK(blocking_of(validate_district(d), "PROP_IN_BUILDING") >= 1, "a bench inside a building must be reported");
}

void test_interior_rules() {
    District d = base_district();
    CHECK(!d.interiors.empty(), "the district has interiors");
    Interior& in = d.interiors[0];
    // A chair through the wall.
    StreetProp chair;
    chair.type = "chair";
    chair.x = in.room.x0 - 0.1;
    chair.y = in.room.cy();
    chair.z = 0.15;
    in.furniture.push_back(chair);
    const ValidationReport r = validate_district(d);
    CHECK(blocking_of(r, "ROOM_FURNITURE_OUTSIDE") >= 1, "furniture through a wall must be reported");

    // A counter on the doorway.
    District e = base_district();
    Interior& ie = e.interiors[0];
    const Building* bd = nullptr;
    for (const Building& b : e.buildings)
        if (b.id == ie.building) bd = &b;
    CHECK(bd != nullptr, "the interior's building exists");
    if (bd != nullptr) {
        const auto dir = leg_dir(bd->front);
        StreetProp counter;
        counter.type = "counter";
        counter.x = bd->door_x - dir[0] * 1.2;
        counter.y = bd->door_y - dir[1] * 1.2;
        counter.z = 0.15;
        counter.scale = 2.0;
        ie.furniture.push_back(counter);
        CHECK(blocking_of(validate_district(e), "DOOR_BLOCKED") >= 1, "furniture in the doorway must be reported");
    }

    // A table on the foot of the stairs (the cafe has them).
    District f = base_district();
    for (Interior& i2 : f.interiors) {
        if (!i2.has_stairs) continue;
        StreetProp table;
        table.type = "table_round";
        table.x = i2.stair_bottom[0];
        table.y = i2.stair_bottom[1];
        table.z = 0.15;
        i2.furniture.push_back(table);
        break;
    }
    CHECK(blocking_of(validate_district(f), "STAIR_BLOCKED") >= 1, "furniture at the foot of the staircase must be reported");
}

// ---- 3. rules the renderer depends on -----------------------------------------------------------------------------------------------------------------------------------------------------
District synthetic_pair() {
    // Two buildings side by side on a south row: A is 10 m wide and 12 m deep, B is 10 m wide and 5 m deep, touching at x = 10. A's west... A's EAST face (x = 10) is covered by B for y in [0, 5] only.
    District d;
    Building a;
    a.id = 1;
    a.footprint = {0, 0, 10, 12};
    Building b;
    b.id = 2;
    b.footprint = {10, 0, 20, 5};
    d.buildings = {a, b};
    return d;
}

void test_exposed_face_spans() {
    const District d = synthetic_pair();
    // A's east face runs y from 0 to 12 (u runs along +y for the east face). B covers y in [0, 5]: the exposed span is [5, 12].
    const auto spans = exposed_face_spans(d, d.buildings[0], Leg::East);
    CHECK(spans.size() == 1, "one exposed span, got %zu", spans.size());
    if (spans.size() == 1) {
        CHECK(std::abs(spans[0].first - 5.0) < 0.1 && std::abs(spans[0].second - 12.0) < 1e-6, "span is [5, 12], got [%.3f, %.3f]", spans[0].first, spans[0].second);
    }
    // B's west face is wholly against A.
    CHECK(exposed_face_spans(d, d.buildings[1], Leg::West).empty(), "B's west face is fully covered");
    // A's other faces are open in full.
    const auto south = exposed_face_spans(d, d.buildings[0], Leg::South);
    CHECK(south.size() == 1 && std::abs(south[0].first) < 1e-9 && std::abs(south[0].second - 10.0) < 1e-6, "A's south face is open end to end");
    // The film's rule dropped A's east face entirely (its midpoint, y = 6, lies 1 m outside it at x = 11, inside B? B spans y in [0, 5]: not inside; so here it keeps it). With B 8 m deep it would drop it.
    District e = d;
    e.buildings[1].footprint = {10, 0, 20, 8};
    CHECK(legacy_face_hidden(e, e.buildings[0], Leg::East), "the film's single-sample rule hides the whole east face when B covers its midpoint");
    const auto spans_e = exposed_face_spans(e, e.buildings[0], Leg::East);
    CHECK(spans_e.size() == 1 && std::abs(spans_e[0].first - 8.0) < 0.1, "...but 4 m of it (y in [8, 12]) is exposed and must be drawn");
}

void test_film_rule_dropped_exposed_walls_on_the_film_seed() {
    // Documents the defect: on the film's seed the old rule removed walls that are visible. Counts faces hidden by the old rule that have at least 0.5 m exposed.
    const District& d = base_district();
    int dropped = 0;
    double metres = 0.0;
    for (const Building& b : d.buildings)
        for (int li = 0; li < 4; ++li) {
            const Leg leg = static_cast<Leg>(li);
            if (!legacy_face_hidden(d, b, leg)) continue;
            double m = 0.0;
            for (const auto& s : exposed_face_spans(d, b, leg)) m += s.second - s.first;
            if (m >= 0.5) {
                ++dropped;
                metres += m;
            }
        }
    std::printf("  film seed 8: the old rule dropped %d faces that have exposed wall, %.1f m in total\n", dropped, metres);
    CHECK(dropped > 0, "the old rule must have dropped exposed walls on the film seed (this is the documented defect)");
}

void test_interior_wall_face_mask() {
    const Rect fp{0, 0, 10, 12};
    // The west wall piece lies on the footprint's west edge: its -x face is the building's outside.
    CHECK(interior_wall_face_mask(fp, {0, 0.3, 0.3, 11.7}) == (63u & ~1u), "west wall drops -x only");
    CHECK(interior_wall_face_mask(fp, {9.7, 0.3, 10, 11.7}) == (63u & ~2u), "east wall drops +x only");
    CHECK(interior_wall_face_mask(fp, {0.3, 11.7, 9.7, 12}) == (63u & ~8u), "north wall drops +y only");
    CHECK(interior_wall_face_mask(fp, {0.3, 0, 9.7, 0.3}) == (63u & ~4u), "south wall drops -y only");
    CHECK(interior_wall_face_mask(fp, {0, 0, 0.3, 0.3}) == (63u & ~1u & ~4u), "a corner piece drops both outside faces");
    CHECK(interior_wall_face_mask(fp, {4, 4, 4.3, 8}) == 63u, "a partition inside the room keeps every face");
    // Every generated interior wall: no kept face lies on the footprint boundary.
    const District& d = base_district();
    for (const Interior& in : d.interiors) {
        const Building* b = nullptr;
        for (const Building& x : d.buildings)
            if (x.id == in.building) b = &x;
        if (b == nullptr) continue;
        for (const Rect& w : in.walls) {
            const unsigned m = interior_wall_face_mask(b->footprint, w);
            if (std::abs(w.x0 - b->footprint.x0) < 1e-4) CHECK((m & 1u) == 0, "kept a face on the west boundary");
            if (std::abs(w.x1 - b->footprint.x1) < 1e-4) CHECK((m & 2u) == 0, "kept a face on the east boundary");
            if (std::abs(w.y0 - b->footprint.y0) < 1e-4) CHECK((m & 4u) == 0, "kept a face on the south boundary");
            if (std::abs(w.y1 - b->footprint.y1) < 1e-4) CHECK((m & 8u) == 0, "kept a face on the north boundary");
        }
    }
}

void test_tree_shape() {
    StreetProp t;
    t.type = "tree";
    t.x = 12.5;
    t.y = -3.25;
    t.z = 0.15;
    t.yaw = 1.0;
    t.scale = 1.0;
    const auto blobs = tree_blobs(12.5f, -3.25f, 0.15f, 1.0f, 1.0f, tree_seed(t));
    // The canopy is leaves at head height and above: a point inside the first blob has negative clearance, a point 20 m away is far outside, and the trunk's own top is inside the canopy.
    CHECK(canopy_clearance(t, blobs[0].x, blobs[0].y, blobs[0].z) < -1.0f, "the centre of the first blob is deep inside");
    CHECK(canopy_clearance(t, 32.5f, -3.25f, 2.0f) > 10.0f, "20 m away is outside");
    float lowest = 1e9f;
    for (const CanopyBlob& b : blobs) lowest = std::min(lowest, b.z - b.r);
    CHECK(lowest > 0.5f && lowest < 3.0f, "the canopy starts between 0.5 and 3 m above the ground (a camera at 1.7-3 m can be inside it): %.2f", lowest);
    // Determinism: the same tree twice.
    const auto again = tree_blobs(12.5f, -3.25f, 0.15f, 1.0f, 1.0f, tree_seed(t));
    CHECK(again[2].x == blobs[2].x && again[4].r == blobs[4].r, "a tree's shape is a pure function of the prop");
}


// ---- camera and canopy rules --------------------------------------------------------------------------------------------------------------------------------------------------------------
void test_trees_and_awnings() {
    const District& d = base_district();
    const std::vector<Box3> awnings = awning_boxes(d);
    CHECK(!awnings.empty(), "commercial fronts have awnings");
    for (const Box3& a : awnings) CHECK(a.z0 > 2.0f && a.z1 <= 3.01f && a.x1 > a.x0 && a.y1 > a.y0, "an awning is a box 2.2 to 3 m above the sidewalk");

    // A tree planted against a facade has its leaves in the building, and the validator says so.
    District e = base_district();
    const Building& b = e.buildings[30];
    const FaceFrame f = face_frame(b.footprint, b.front);
    StreetProp tree;
    tree.type = "tree";
    tree.x = f.origin[0] + f.u[0] * 0.5 * f.length + f.out[0] * 0.8;
    tree.y = f.origin[1] + f.u[1] * 0.5 * f.length + f.out[1] * 0.8;
    tree.z = 0.15;
    tree.scale = 1.0;
    CHECK(!tree_clear_of_buildings(e, tree), "a tree 0.8 m from a facade does not clear it");
    e.props.push_back(tree);
    const ValidationReport r = validate_district(e);
    CHECK(blocking_of(r, "TREE_CANOPY_ON_BUILDING") + blocking_of(r, "TREE_CANOPY_ON_AWNING") >= 1, "leaves through a wall or an awning must be reported");
    // Far from any building the same tree is fine.
    StreetProp far_tree = tree;
    far_tree.x = 0.0;
    far_tree.y = 0.0;   // the middle of the main intersection
    CHECK(tree_clear_of_buildings(e, far_tree, 0.3f), "a tree in the open clears everything");
}

void test_view_obstruction() {
    const District& d = base_district();
    const std::vector<Box3> awnings = awning_boxes(d);
    std::vector<StreetProp> trees;
    for (const StreetProp& p : d.props)
        if (p.type == "tree") trees.push_back(p);
    CHECK(!trees.empty(), "the district has trees");
    // 1. A camera in the open street looking along it is not obstructed.
    CHECK(!view_obstructed(trees, awnings, {0.0f, 0.0f, 2.0f}, {0.0f, -30.0f, 1.5f}), "open road: not obstructed");
    // 2. The eye inside a canopy is obstructed; the same camera 8 m away looking away is not.
    const StreetProp& t = trees.front();
    const auto blobs = tree_blobs(static_cast<float>(t.x), static_cast<float>(t.y), static_cast<float>(t.z), static_cast<float>(t.yaw), static_cast<float>(t.scale), tree_seed(t));
    CHECK(view_obstructed(trees, {}, {blobs[0].x, blobs[0].y, blobs[0].z}, {blobs[0].x + 10.0f, blobs[0].y, blobs[0].z}), "the eye inside leaves is obstructed");
    // 3. Under a shopfront awning, looking along the street with the awning's edge across the top of the frame: obstructed. The same camera two metres further out: not.
    const Box3& a = awnings.front();
    const bool long_x = (a.x1 - a.x0) > (a.y1 - a.y0);
    const float cx = 0.5f * (a.x0 + a.x1), cy = 0.5f * (a.y0 + a.y1);
    const std::array<float, 3> under = long_x ? std::array<float, 3>{cx - 3.0f, cy, 2.05f} : std::array<float, 3>{cx, cy - 3.0f, 2.05f};
    const std::array<float, 3> along = long_x ? std::array<float, 3>{cx + 6.0f, cy, 1.4f} : std::array<float, 3>{cx, cy + 6.0f, 1.4f};
    CHECK(view_obstructed({}, awnings, under, along, 52.0f), "an awning across the top of the frame obstructs the view");
    const std::array<float, 3> high = {under[0], under[1], 3.6f};
    CHECK(!view_obstructed({}, awnings, high, {along[0], along[1], 3.2f}, 52.0f), "above the awning looking along the street is not obstructed");
}


// ---- render / collision / navigation consistency --------------------------------------------------------------------------------------------------------------------------------------------
void test_consistency_rules() {
    // Generated districts: every lane lies on the drawn roadway (the entry lanes used to start 15 m beyond its end).
    const District& d = base_district();
    for (const District::LanePath& lp : d.lane_paths) {
        bool a = false, b = false;
        for (const Rect& r : d.roadway) {
            a = a || (r.x0 - 0.5 <= lp.a[0] && lp.a[0] <= r.x1 + 0.5 && r.y0 - 0.5 <= lp.a[1] && lp.a[1] <= r.y1 + 0.5);
            b = b || (r.x0 - 0.5 <= lp.b[0] && lp.b[0] <= r.x1 + 0.5 && r.y0 - 0.5 <= lp.b[1] && lp.b[1] <= r.y1 + 0.5);
        }
        CHECK(a && b, "lane %u has an end off the roadway", lp.id);
    }
    // A lane that leaves the road is reported.
    District e = base_district();
    e.lane_paths.push_back({9999, {e.roadway[0].x0 + 3.0, e.roadway[0].y0 - 30.0}, {e.roadway[0].x0 + 3.0, e.roadway[0].y0 - 20.0}, false});
    CHECK(blocking_of(validate_district(e), "LANE_OFF_ROAD") >= 1, "a lane off the road must be reported");
    // A pedestrian node on the roadway outside a crosswalk is reported; so is an edge through a building.
    District f = base_district();
    PedNode on_road;
    on_road.x = f.roadway[3].cx();
    on_road.y = f.roadway[3].cy();
    f.ped_nodes.push_back(on_road);
    CHECK(blocking_of(validate_district(f), "NAV_NODE_ON_ROAD") >= 1, "a walker node in the road must be reported");
    District g = base_district();
    const Building* bld = nullptr;
    for (const Building& b : g.buildings)
        if (!b.interior) { bld = &b; break; }
    PedNode n1, n2;
    n1.x = bld->footprint.x0 - 1.0;
    n1.y = bld->footprint.cy();
    n2.x = bld->footprint.x1 + 1.0;
    n2.y = bld->footprint.cy();
    g.ped_nodes.push_back(n1);
    g.ped_nodes.push_back(n2);
    g.ped_edges.push_back({static_cast<std::uint32_t>(g.ped_nodes.size() - 2), static_cast<std::uint32_t>(g.ped_nodes.size() - 1), 0});
    CHECK(blocking_of(validate_district(g), "NAV_EDGE_THROUGH_BUILDING") >= 1, "an edge through a building must be reported");
    // A building whose collision is removed is reported (a drawn building the walkers and cars would enter).
    District h = base_district();
    const Rect fp = h.buildings[40].footprint;
    std::erase_if(h.collision, [&](const saige::Aabb& c) { return static_cast<double>(c.min.x) <= fp.cx() && fp.cx() <= static_cast<double>(c.max.x) && static_cast<double>(c.min.y) <= fp.cy() && fp.cy() <= static_cast<double>(c.max.y) && c.max.z > 3.0f; });
    CHECK(blocking_of(validate_district(h), "COLLISION_MISSING_BUILDING") >= 1, "a building without collision must be reported");
    // The director's report that crosswalks end at raised kerbs is a fact about the data: every crosswalk is reported until ramps exist (a warning, not blocking).
    const ValidationReport r = validate_district(d);
    CHECK(r.count("CROSSWALK_CURB_STEP") == d.crosswalks.size() || !d.curb_ramps.empty(), "every crosswalk without a ramp is reported (%zu of %zu)", r.count("CROSSWALK_CURB_STEP"), d.crosswalks.size());
}

} // namespace

int main() {
    test_generated_districts_are_clean();
    test_building_overlap_and_road();
    test_coplanar_walls();
    test_props_on_road_and_in_parking();
    test_prop_in_building();
    test_seat_facing();
    test_hazard_lamps();
    test_hazard_surface_anchors();
    test_interior_rules();
    test_exposed_face_spans();
    test_film_rule_dropped_exposed_walls_on_the_film_seed();
    test_interior_wall_face_mask();
    test_tree_shape();
    test_trees_and_awnings();
    test_view_obstruction();
    test_consistency_rules();
    if (failures != 0) {
        std::printf("validate_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("validate_test ok\n");
    return 0;
}
