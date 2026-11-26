// Showcase 04: district validation tests. Two kinds: (1) the generated districts are clean (no finding of error severity on 16 seeds), and (2) the validator is not vacuous: each defect class is
// injected into a clean district and must be reported, with the code and ids an editor would act on. Plus the geometry rules the renderer depends on (exposed spans of a wall, the
// interior wall face mask, the shared tree shape). A passing run of this file does not say the film looks right; the visual regression script does (tools/visual_regression.py).
#include "district_validate.hpp"

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
