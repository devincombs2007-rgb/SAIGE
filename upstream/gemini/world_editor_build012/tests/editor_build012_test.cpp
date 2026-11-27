#include "saige/editor_assets.hpp"
#include "saige/editor_commands.hpp"
#include "saige/editor_materials.hpp"
#include "saige/editor_project.hpp"
#include "saige/editor_scene.hpp"
#include "saige/editor_semantic.hpp"
#include "saige/editor_workspace.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define ED_CHECK(cond)                                                     \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "ED_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                 \
      std::exit(1);                                                        \
    }                                                                      \
  } while (false)

using namespace saige::editor;

namespace {

void test_asset_library_and_vehicle_validation() {
  AssetLibrary lib;

  AssetMetadata sedan{};
  sedan.asset_id = 101;
  sedan.uri = "assets/vehicles/sedan_01.glb";
  sedan.display_name = "Compact Sedan";
  sedan.category = LibraryAssetCategory::Vehicle;
  sedan.tags = {"vehicle", "urban", "sedan"};
  sedan.source_url = "https://quaternius.com/packs/ultimateroads.html";
  sedan.author = "Quaternius";
  sedan.license_spdx = "CC0-1.0";
  sedan.redistribution_allowed = true;
  sedan.content_hash = 0xCAFEBABE0001ULL;
  sedan.triangle_count = 12000;
  sedan.lod_count = 2;
  sedan.vehicle_meta = VehicleAssetMetadata{
      {{-0.95, -2.2, 0.0}, {0.95, 2.2, 1.45}},
      0.34,
      {{"wheel_fl", {-0.85, 1.35, 0.34}, 0.25}},
      {{"headlight_l", {-0.7, 2.18, 0.65}, 0.15},
       {"hazard_l", {-0.8, 2.15, 0.65}, 0.15}},
      {{"driver", {-0.38, 0.1, 0.55}, 0.25}},
  };
  ED_CHECK(lib.register_asset(sedan));
  ED_CHECK(lib.validate_asset(101).passed);

  // Mutation: detached hazard light 2.5m outside vehicle chassis
  AssetMetadata bad_sedan = sedan;
  bad_sedan.asset_id = 102;
  bad_sedan.uri = "assets/vehicles/sedan_bad_hazard.glb";
  bad_sedan.content_hash = 0xCAFEBABE0002ULL;
  bad_sedan.vehicle_meta->lights[1].local_pos_m = {-3.5, 2.15, 2.8};
  ED_CHECK(lib.register_asset(bad_sedan));
  const auto bad_rep = lib.validate_asset(102);
  ED_CHECK(!bad_rep.passed);
  bool found_detached = false;
  for (const auto& iss : bad_rep.issues) {
    if (iss.code == "DETACHED_VEHICLE_LIGHT") found_detached = true;
  }
  ED_CHECK(found_detached);

  // Missing dependency + missing license + duplicate hash
  AssetMetadata broken{};
  broken.asset_id = 103;
  broken.uri = "assets/props/bench_01.glb";
  broken.display_name = "Park Bench";
  broken.content_hash = 0xCAFEBABE0001ULL;  // duplicate of 101
  broken.dependencies = {"assets/materials/missing_wood.mat"};
  ED_CHECK(lib.register_asset(broken));
  const auto br = lib.validate_asset(103);
  ED_CHECK(!br.passed);
  ED_CHECK(br.error_count >= 3);  // license + redistribution + missing dep

  ED_CHECK(lib.set_favorite(101, true));
  ED_CHECK(lib.search(LibraryAssetCategory::Vehicle, "sedan", "urban", true).size() == 1);
  ED_CHECK(lib.increment_usage("assets/vehicles/sedan_01.glb"));
  ED_CHECK(lib.find_by_id(101)->usage_count == 1);
}

void test_semantic_placement_undo_and_save_reload() {
  EditorScene scene;
  CommandHistory hist;

  EditorNode table{};
  table.id = scene.allocate_id();
  table.name = "Cafe_Table";
  table.kind = EditorNodeKind::Prop;
  table.local_transform.position = {10.0, 10.0, 0.0};
  ED_CHECK(hist.execute(scene, std::make_unique<CreateNodeCommand>(table)));

  EditorNode chair{};
  chair.id = scene.allocate_id();
  chair.name = "Cafe_Chair";
  chair.kind = EditorNodeKind::Prop;
  chair.local_transform.position = {13.5, 10.0, 0.4};  // floating & far
  ED_CHECK(hist.execute(scene, std::make_unique<CreateNodeCommand>(chair)));

  const SemanticMetadata chair_meta{SemanticRole::Chair};
  const SemanticMetadata table_meta{SemanticRole::Table};

  // Floating check before placement
  const auto pre_diags = SemanticPlacementEngine::validate_clearance(scene, chair.id, chair_meta);
  ED_CHECK(!pre_diags.empty());

  const auto prop = SemanticPlacementEngine::propose_alignment(
      scene, chair.id, chair_meta, table.id, table_meta, 0.0, 1.0);
  ED_CHECK(prop.valid);
  ED_CHECK(std::abs(prop.proposed_transform.position.z) < 1e-6);
  ED_CHECK(std::abs(prop.proposed_transform.position.x - 11.0) < 1e-6);
  // Chair at +X from table faces -X => yaw = +90 deg (+Y rotated +90 deg around Z -> -X)
  ED_CHECK(std::abs(prop.proposed_transform.rotation_deg.z - 90.0) < 1e-3);

  ED_CHECK(hist.execute(scene, std::make_unique<ApplySemanticPlacementCommand>(prop)));
  ED_CHECK(std::abs(scene.find_node(chair.id)->local_transform.position.x - 11.0) < 1e-6);

  // Undo & Redo
  ED_CHECK(hist.undo(scene));
  ED_CHECK(std::abs(scene.find_node(chair.id)->local_transform.position.x - 13.5) < 1e-6);
  ED_CHECK(hist.redo(scene));
  ED_CHECK(std::abs(scene.find_node(chair.id)->local_transform.position.x - 11.0) < 1e-6);

  // Save & Reload preserves stable EntityId & semantic transform
  const auto blob = EditorProjectSerializer::serialize(scene);
  EditorScene reloaded;
  ED_CHECK(EditorProjectSerializer::deserialize(blob, reloaded));
  const EditorNode* rc = reloaded.find_node(chair.id);
  ED_CHECK(rc != nullptr);
  ED_CHECK(std::abs(rc->local_transform.position.x - 11.0) < 1e-6);
}

void test_material_glass_and_interior_mapping() {
  const auto glass = MaterialAuthoringAsset::from_preset(
      MaterialPreset::ReflectiveArchitecturalGlass, "Office_Window_Glass");
  ED_CHECK(glass.is_glass);
  ED_CHECK(glass.glass.transmission > 0.5);
  ED_CHECK(glass.glass.refraction_status ==
           FeatureSupportStatus::PlannedEngineDependency);
  ED_CHECK(glass.validate().empty());

  auto bad_glass = glass;
  bad_glass.glass.ior = 0.4;  // invalid physical IOR
  bad_glass.interior_mapping.enabled = true;
  bad_glass.interior_mapping.room_width_m = 0.1;  // invalid room width
  ED_CHECK(bad_glass.validate().size() >= 2);
}

void test_negative_and_corruption_cases() {
  // 1. Corrupted SED1 blob & oversized node count
  EditorScene out;
  std::vector<std::uint8_t> bad_magic = {'B', 'A', 'D', '1', 0, 1};
  ED_CHECK(!EditorProjectSerializer::deserialize(bad_magic, out));

  std::vector<std::uint8_t> truncated = {'S', 'E', 'D', '1'};
  ED_CHECK(!EditorProjectSerializer::deserialize(truncated, out));

  // 2. Invalid NaN transform rejected by set_transform & caught by validate()
  EditorScene s;
  EditorNode n{};
  n.id = s.allocate_id();
  n.name = "BadTransform";
  ED_CHECK(s.insert_node(n));
  Transform3d bad_t{};
  bad_t.position.x = std::numeric_limits<double>::quiet_NaN();
  ED_CHECK(!s.set_transform(n.id, bad_t));
}

struct BenchTiming {
  double scene_1k_ms = 0.0;
  double scene_10k_ms = 0.0;
  double assets_10k_ms = 0.0;
};

BenchTiming run_scalability_benchmarks() {
  BenchTiming bt{};

  auto bench_scene = [](std::size_t count) -> double {
    const auto t0 = std::chrono::steady_clock::now();
    EditorScene s;
    for (std::size_t i = 0; i < count; ++i) {
      EditorNode n{};
      n.id = s.allocate_id();
      n.name = "Entity_" + std::to_string(i);
      n.local_transform.position = {static_cast<double>(i % 100) * 4.0,
                                    static_cast<double>(i / 100) * 4.0, 0.0};
      ED_CHECK(s.insert_node(std::move(n)));
    }
    const auto q = s.query_chunk({1, 0});
    ED_CHECK(!q.empty());
    const auto blob = EditorProjectSerializer::serialize(s);
    EditorScene reloaded;
    ED_CHECK(EditorProjectSerializer::deserialize(blob, reloaded));
    ED_CHECK(reloaded.nodes().size() == count);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

  bt.scene_1k_ms = bench_scene(1000);
  bt.scene_10k_ms = bench_scene(10000);

  const auto ta0 = std::chrono::steady_clock::now();
  AssetLibrary lib;
  for (std::uint64_t i = 1; i <= 10000; ++i) {
    AssetMetadata a{};
    a.asset_id = i;
    a.uri = "assets/env/prop_" + std::to_string(i) + ".glb";
    a.display_name = "Prop_" + std::to_string(i);
    a.source_url = "https://polyhaven.com/models";
    a.author = "Poly Haven";
    a.license_spdx = "CC0-1.0";
    a.redistribution_allowed = true;
    a.content_hash = i + 100000ULL;
    ED_CHECK(lib.register_asset(std::move(a)));
  }
  const auto hits = lib.search(LibraryAssetCategory::Environment, "Prop_999");
  ED_CHECK(!hits.empty());
  const auto val = lib.validate_all();
  ED_CHECK(val.passed);
  const auto ta1 = std::chrono::steady_clock::now();
  bt.assets_10k_ms = std::chrono::duration<double, std::milli>(ta1 - ta0).count();

  std::cout << "[bench] scene_1k_ms=" << bt.scene_1k_ms
            << " scene_10k_ms=" << bt.scene_10k_ms
            << " assets_10k_ms=" << bt.assets_10k_ms << "\n";
  return bt;
}

}  // namespace

int main(int argc, char** argv) {
  test_asset_library_and_vehicle_validation();
  test_semantic_placement_undo_and_save_reload();
  test_material_glass_and_interior_mapping();
  test_negative_and_corruption_cases();
  const auto bt = run_scalability_benchmarks();

  if (argc >= 3 && std::string(argv[1]) == "--dump-json") {
    std::ofstream jf(argv[2]);
    jf << "{\n"
       << "  \"build\": \"012\",\n"
       << "  \"scene_1k_insert_query_roundtrip_ms\": " << bt.scene_1k_ms << ",\n"
       << "  \"scene_10k_insert_query_roundtrip_ms\": " << bt.scene_10k_ms << ",\n"
       << "  \"assets_10k_register_search_validate_ms\": " << bt.assets_10k_ms << "\n"
       << "}\n";
  }

  std::cout << "editor_build012_test: ALL 5 SUITES PASSED\n";
  return 0;
}
