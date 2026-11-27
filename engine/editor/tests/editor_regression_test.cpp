// Regression tests for the findings of director review 7198676267 on World Editor Build 012 (2026-11-27). Each case failed on the delivered code
// (measured with a probe against upstream/gemini/world_editor_build012) and passes on the integrated, corrected copy.
#include "saige/editor_assets.hpp"
#include "saige/editor_materials.hpp"
#include "saige/editor_project.hpp"
#include "saige/editor_scene.hpp"
#include "saige/editor_semantic.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace saige::editor;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}
bool has_code(const std::vector<ValidationDiagnostic>& d, const char* code)
{
    for (const auto& x : d)
        if (x.code == code) return true;
    return false;
}
bool has_issue(const AssetValidationReport& r, const char* code)
{
    for (const auto& x : r.issues)
        if (x.code == code) return true;
    return false;
}
EntityId add(EditorScene& s, const char* name, Vec3d pos, EntityId parent = kInvalidEntityId, Vec3d rot_deg = {})
{
    EditorNode n;
    n.id = s.allocate_id();
    n.name = name;
    n.parent_id = parent;
    n.local_transform.position = pos;
    n.local_transform.rotation_deg = rot_deg;
    n.chunk = s.compute_chunk_for_position(pos);
    const EntityId id = n.id;
    check(s.insert_node(n), "a node is inserted");
    return id;
}
const double kNaN = std::numeric_limits<double>::quiet_NaN();

void test_material_ranges()
{
    MaterialAuthoringAsset g = MaterialAuthoringAsset::from_preset(MaterialPreset::ClearGlass, "glass");
    check(g.validate().empty(), "the clear glass preset is valid");
    auto bad = g;
    bad.glass.opacity = 5.0;
    check(has_code(bad.validate(), "INVALID_GLASS_OPACITY"), "glass opacity 5 is an error");
    bad = g;
    bad.glass.transmission = -1.0;
    check(has_code(bad.validate(), "INVALID_GLASS_TRANSMISSION"), "glass transmission -1 is an error");
    bad = g;
    bad.glass.thickness_m = -0.01;
    check(has_code(bad.validate(), "INVALID_GLASS_THICKNESS"), "a negative glass thickness is an error");
    bad = g;
    bad.glass.tint_rgb = {1.5, 0.5, 0.5};
    check(has_code(bad.validate(), "INVALID_GLASS_TINT"), "a tint above 1 is an error");
    MaterialAuthoringAsset room = MaterialAuthoringAsset::from_preset(MaterialPreset::Concrete, "room");
    room.interior_mapping.enabled = true;
    check(room.validate().empty(), "a default interior is valid");
    room.interior_mapping.room_depth_m = kNaN;  // Build 012: NaN <= 0.5 is false, so it passed
    check(has_code(room.validate(), "INVALID_INTERIOR_ROOM_DIMS"), "a NaN room depth is an error");
    room.interior_mapping.room_depth_m = 4.0;
    room.interior_mapping.day_night_blend = 2.0;
    check(has_code(room.validate(), "INVALID_INTERIOR_BLEND"), "a day/night blend of 2 is an error");
}

void test_clearance_across_chunks_and_in_world_space()
{
    SemanticMetadata m;
    m.clearance_radius_m = 0.5;
    m.min_walkway_clearance_m = 0.0;
    {
        EditorScene s;
        const EntityId a = add(s, "a", {63.9, 10.0, 0.0});
        add(s, "b", {64.1, 10.0, 0.0});  // another chunk, 0.2 m away
        check(has_code(SemanticPlacementEngine::validate_clearance(s, a, m), "CLEARANCE_VIOLATION"), "objects 0.2 m apart across x = 64 m are compared");
    }
    {
        EditorScene s;
        const EntityId parent = add(s, "parent", {10.0, 0.0, 0.0});
        const EntityId child = add(s, "child", {1.0, 0.0, 0.0}, parent);  // world x = 11
        add(s, "neighbour", {11.0, 0.0, 0.0});
        check(has_code(SemanticPlacementEngine::validate_clearance(s, child, m), "CLEARANCE_VIOLATION"), "a child is checked where it is in the world");
        Vec3d w{};
        check(s.world_position(child, w) && std::abs(w.x - 11.0) < 1e-9, "the child's world position composes its parent's");
        check(!has_code(SemanticPlacementEngine::validate_clearance(s, parent, m), "CLEARANCE_VIOLATION"), "a parent is not compared with its own child");
    }
    {
        EditorScene s;  // a rotated parent: a child 2 m along local +X of a parent yawed 90 degrees sits 2 m along world +Y
        const EntityId parent = add(s, "parent", {0.0, 0.0, 0.0}, kInvalidEntityId, {0.0, 0.0, 90.0});
        const EntityId child = add(s, "child", {2.0, 0.0, 0.0}, parent);
        Vec3d w{};
        check(s.world_position(child, w) && std::abs(w.x) < 1e-9 && std::abs(w.y - 2.0) < 1e-9, "rotation of the parent carries the child");
    }
    {
        EditorScene s;  // ground height: an object on a 5 m terrace is grounded when the ground is at 5 m
        const EntityId a = add(s, "on_terrace", {0.0, 0.0, 5.0});
        SemanticMetadata g;
        check(has_code(SemanticPlacementEngine::validate_clearance(s, a, g, 0.0), "FLOATING_OBJECT"), "5 m above z = 0 floats");
        check(!has_code(SemanticPlacementEngine::validate_clearance(s, a, g, 5.0), "FLOATING_OBJECT"), "on a 5 m ground it does not");
    }
    {
        EditorScene s;  // the walkway clearance is used: a 1.2 m centre distance with a 0.5 m radius leaves 0.7 m, below a 1 m walkway
        const EntityId a = add(s, "bench", {0.0, 0.0, 0.0});
        add(s, "bin", {1.2, 0.0, 0.0});
        SemanticMetadata w;
        w.clearance_radius_m = 0.5;
        w.min_walkway_clearance_m = 1.0;
        check(has_code(SemanticPlacementEngine::validate_clearance(s, a, w), "WALKWAY_NARROW"), "a gap narrower than the walkway is reported");
        w.min_walkway_clearance_m = 0.5;
        check(!has_code(SemanticPlacementEngine::validate_clearance(s, a, w), "WALKWAY_NARROW"), "a wide enough gap is not");
    }
}

void test_asset_provenance_and_vehicle_checks()
{
    AssetLibrary lib;
    AssetMetadata a;
    a.asset_id = 7;
    a.uri = "asset://car";
    a.display_name = "car";
    a.category = LibraryAssetCategory::Vehicle;
    a.source_url = "not a url";
    a.license_spdx = "TOTALLY-FREE";
    a.redistribution_allowed = true;
    a.dependencies = {"asset://car"};
    VehicleAssetMetadata vm;
    vm.wheel_radius_m = -0.3;
    vm.seats.push_back({"driver_seat", {0.0, 0.0, 9.0}, 0.25});
    a.vehicle_meta = vm;
    check(lib.register_asset(a), "the record registers (validation is separate)");
    const auto rep = lib.validate_asset(7);
    check(!rep.passed, "Build 012 passed this record with no error; it must fail");
    check(has_issue(rep, "UNRECOGNISED_LICENSE"), "an unknown licence id is an error");
    check(has_issue(rep, "INVALID_SOURCE_URL"), "a source that is not a URL is an error");
    check(has_issue(rep, "MISSING_AUTHOR"), "a missing author is an error");
    check(has_issue(rep, "SELF_DEPENDENCY"), "a self dependency is an error");
    check(has_issue(rep, "INVALID_WHEEL_RADIUS"), "a negative wheel radius is an error");
    check(has_issue(rep, "DETACHED_VEHICLE_SEAT"), "a seat 9 m above the body is an error");
    check(has_issue(rep, "PROVENANCE_UNVERIFIED"), "declared provenance is reported as unverified");

    AssetMetadata good;
    good.asset_id = 8;
    good.uri = "asset://lamp";
    good.display_name = "street lamp";
    good.category = LibraryAssetCategory::Environment;
    good.source_url = "https://polyhaven.com/a/street_lamp_01";
    good.author = "Poly Haven";
    good.license_spdx = "CC0-1.0";
    good.redistribution_allowed = true;
    good.provenance_verified = true;
    check(lib.register_asset(good), "a good record registers");
    const auto ok = lib.validate_asset(8);
    check(ok.passed && ok.error_count == 0, "a CC0 record with a real source and author passes");
    AssetMetadata nc = good;
    nc.asset_id = 9;
    nc.uri = "asset://nc";
    nc.license_spdx = "CC-BY-NC-4.0";
    check(lib.register_asset(nc), "an NC record registers");
    check(has_issue(lib.validate_asset(9), "UNRECOGNISED_LICENSE"), "a non-commercial licence is refused");
}

void test_front_axis_convention()
{
    EditorScene s;
    const EntityId chair = add(s, "chair", {0.0, 0.0, 0.0});
    const EntityId table = add(s, "table", {3.0, 0.0, 0.0});
    SemanticMetadata cm;
    cm.role = SemanticRole::Chair;
    SemanticMetadata tm;
    tm.role = SemanticRole::Table;
    const auto p = SemanticPlacementEngine::propose_alignment(s, chair, cm, table, tm);
    check(std::abs(p.proposed_transform.rotation_deg.z) < 1e-6, "with the engine's +X front, a chair looking along +X has yaw 0 (Build 012: -90)");
    SemanticMetadata authored_y = cm;
    authored_y.local_front_dir = {0.0, 1.0, 0.0};
    const auto q = SemanticPlacementEngine::propose_alignment(s, chair, authored_y, table, tm);
    check(std::abs(q.proposed_transform.rotation_deg.z + 90.0) < 1e-6, "an asset authored with a +Y front is turned -90 degrees, as stated in its metadata");
}

void test_semantic_metadata_persists_with_the_project()
{
    EditorScene s;
    const EntityId chair = add(s, "chair", {1.0, 2.0, 0.0});
    SemanticMetadata m;
    m.role = SemanticRole::Chair;
    m.local_front_dir = {0.0, 1.0, 0.0};
    m.clearance_radius_m = 0.4;
    m.min_walkway_clearance_m = 0.9;
    m.requires_ground_contact = false;
    check(store_semantic_metadata(s, chair, m), "valid metadata is stored");
    const auto blob = EditorProjectSerializer::serialize(s);
    EditorScene back;
    check(EditorProjectSerializer::deserialize(blob, back), "the project loads");
    const EditorNode* n = back.find_node(chair);
    const auto loaded = n ? load_semantic_metadata(*n) : std::nullopt;
    check(loaded.has_value(), "the metadata survives a save and load");
    check(loaded && loaded->role == SemanticRole::Chair && loaded->local_front_dir == m.local_front_dir && loaded->clearance_radius_m == 0.4 &&
              loaded->min_walkway_clearance_m == 0.9 && !loaded->requires_ground_contact,
          "every field survives");
    SemanticMetadata bad = m;
    bad.local_front_dir = {0.0, 0.0, 1.0};  // no horizontal component: no yaw can be derived
    check(!store_semantic_metadata(s, chair, bad), "a vertical front axis is refused");
    bad = m;
    bad.clearance_radius_m = kNaN;
    check(!store_semantic_metadata(s, chair, bad), "a NaN clearance is refused");
    const EntityId plain_id = add(s, "plain", {5.0, 5.0, 0.0});
    const EditorNode* plain = s.find_node(plain_id);
    check(plain && !load_semantic_metadata(*plain).has_value(), "a node without metadata has none");
}
}  // namespace

int main()
{
    test_material_ranges();
    test_clearance_across_chunks_and_in_world_space();
    test_asset_provenance_and_vehicle_checks();
    test_front_axis_convention();
    test_semantic_metadata_persists_with_the_project();
    std::printf("editor_regression: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
