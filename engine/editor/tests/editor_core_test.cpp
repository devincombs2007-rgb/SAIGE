#include "saige/editor_commands.hpp"
#include "saige/editor_project.hpp"
#include "saige/editor_scene.hpp"
#include "saige/editor_workspace.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::editor;

namespace {

void test_hierarchy_reparenting_and_cycle_guard() {
  EditorScene scene(64.0);
  CommandHistory history;

  EditorNode root{};
  root.name = "District_01";
  root.kind = EditorNodeKind::Group;
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(root)));

  EditorNode bldg{};
  bldg.parent_id = 1;
  bldg.name = "Cafe_01";
  bldg.kind = EditorNodeKind::Building;
  bldg.asset.asset_uri = "assets/buildings/cafe_01.saige_asset";
  bldg.local_transform.position = {130.0, -70.0, 2.0};
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(bldg)));

  const EditorNode* n2 = scene.find_node(2);
  SAIGE_CHECK(n2 != nullptr);
  SAIGE_CHECK(n2->chunk == (ChunkCoord{2, -2}));
  SAIGE_CHECK(!history.execute(scene, std::make_unique<ReparentCommand>(1, 2)));
}

void test_snapping_duplication_and_grouping() {
  EditorScene scene(64.0);
  CommandHistory history;
  scene.snap_settings() = {true, 0.5, true, 15.0};

  EditorNode prop{};
  prop.name = "LampPost";
  prop.kind = EditorNodeKind::Prop;
  prop.asset.asset_uri = "assets/props/lamp.saige_asset";
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(prop)));

  Transform3d raw{{10.23, 4.78, 0.12}, {0.0, 0.0, 22.0}, {1.0, 1.0, 1.0}};
  SAIGE_CHECK(history.execute(scene, std::make_unique<SetTransformCommand>(1, raw, true)));
  const auto* n1 = scene.find_node(1);
  SAIGE_CHECK(std::abs(n1->local_transform.position.x - 10.0) < 1e-6);
  SAIGE_CHECK(std::abs(n1->local_transform.position.y - 5.0) < 1e-6);
  SAIGE_CHECK(std::abs(n1->local_transform.rotation_deg.z - 15.0) < 1e-6);

  auto dup = std::make_unique<DuplicateSubtreeCommand>(1, Vec3d{5.0, 0.0, 0.0});
  auto* dup_ptr = dup.get();
  SAIGE_CHECK(history.execute(scene, std::move(dup)));
  const EntityId dup_id = dup_ptr->duplicated_root_id();
  SAIGE_CHECK(scene.find_node(dup_id) != nullptr);
  SAIGE_CHECK(std::abs(scene.find_node(dup_id)->local_transform.position.x - 15.0) < 1e-6);

  auto grp = std::make_unique<GroupNodesCommand>("StreetLights", std::vector<EntityId>{1, dup_id});
  auto* grp_ptr = grp.get();
  SAIGE_CHECK(history.execute(scene, std::move(grp)));
  const EntityId grp_id = grp_ptr->group_id();
  SAIGE_CHECK(scene.find_node(1)->parent_id == grp_id);
  SAIGE_CHECK(scene.find_node(dup_id)->parent_id == grp_id);
}

void test_transactional_undo_redo_and_rollback() {
  EditorScene scene;
  CommandHistory history;

  SAIGE_CHECK(history.begin_transaction("PlaceBlock"));
  for (int i = 0; i < 3; ++i) {
    EditorNode n{};
    n.name = "Prop_" + std::to_string(i);
    n.kind = EditorNodeKind::Prop;
    n.asset.asset_uri = "assets/props/crate.saige_asset";
    SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(n)));
  }
  SAIGE_CHECK(history.commit_transaction());
  SAIGE_CHECK(scene.nodes().size() == 3);
  SAIGE_CHECK(history.undo_count() == 1);

  SAIGE_CHECK(history.undo(scene));
  SAIGE_CHECK(scene.nodes().empty());

  SAIGE_CHECK(history.redo(scene));
  SAIGE_CHECK(scene.nodes().size() == 3);

  SAIGE_CHECK(history.begin_transaction("Partial"));
  EditorNode extra{};
  extra.name = "Temp";
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(extra)));
  SAIGE_CHECK(scene.nodes().size() == 4);
  SAIGE_CHECK(history.rollback_transaction(scene));
  SAIGE_CHECK(scene.nodes().size() == 3);
}

void test_delete_subtree_undo_and_locked_protection() {
  EditorScene scene;
  CommandHistory history;

  EditorNode parent{};
  parent.name = "Building";
  parent.kind = EditorNodeKind::Building;
  parent.asset.asset_uri = "assets/bldg.saige_asset";
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(parent)));

  EditorNode child{};
  child.parent_id = 1;
  child.name = "Door";
  child.kind = EditorNodeKind::Prop;
  child.asset.asset_uri = "assets/door.saige_asset";
  SAIGE_CHECK(history.execute(scene, std::make_unique<CreateNodeCommand>(child)));

  SAIGE_CHECK(history.execute(scene, std::make_unique<DeleteSubtreeCommand>(1)));
  SAIGE_CHECK(scene.nodes().empty());

  SAIGE_CHECK(history.undo(scene));
  SAIGE_CHECK(scene.nodes().size() == 2);
  SAIGE_CHECK(scene.find_node(2)->parent_id == 1);

  scene.find_node(1)->locked = true;
  SAIGE_CHECK(!history.execute(scene, std::make_unique<SetTransformCommand>(1, Transform3d{})));
  SAIGE_CHECK(!history.execute(scene, std::make_unique<DeleteSubtreeCommand>(1)));
}

void test_scene_validation_diagnostics() {
  EditorScene scene;
  EditorNode missing_uri{};
  missing_uri.id = 1;
  missing_uri.name = "MissingAsset";
  missing_uri.kind = EditorNodeKind::Vegetation;
  SAIGE_CHECK(scene.insert_node(missing_uri));

  EditorNode bad_scale{};
  bad_scale.id = 2;
  bad_scale.name = "BadScale";
  bad_scale.kind = EditorNodeKind::Group;
  bad_scale.local_transform.scale = {0.0, 1.0, 1.0};
  SAIGE_CHECK(scene.insert_node(bad_scale));

  const auto diags = scene.validate();
  SAIGE_CHECK(diags.size() == 2);
  SAIGE_CHECK(diags[0].code == "MISSING_ASSET_URI");
  SAIGE_CHECK(diags[1].code == "INVALID_TRANSFORM");
}

void test_project_serialization_and_corruption_guards() {
  EditorScene scene(64.0);
  EditorNode n{};
  n.id = 10;
  n.name = "HeroBuilding";
  n.kind = EditorNodeKind::Building;
  n.local_transform.position = {120.0, 250.0, 4.5};
  n.asset = {"assets/hero.saige_asset", 3, 0x123456789ABCDEF0ULL};
  SAIGE_CHECK(scene.insert_node(n));
  SAIGE_CHECK(scene.set_property(10, "floors", PropertyValue{PropertyType::Int64, false, 4, 0.0, "", {}}));

  const auto blob = EditorProjectSerializer::serialize(scene);
  SAIGE_CHECK(!blob.empty());

  EditorScene restored;
  SAIGE_CHECK(EditorProjectSerializer::deserialize(blob, restored));
  SAIGE_CHECK(restored.nodes().size() == 1);
  const auto* rn = restored.find_node(10);
  SAIGE_CHECK(rn != nullptr);
  SAIGE_CHECK(rn->name == "HeroBuilding");
  SAIGE_CHECK(rn->asset.asset_version == 3);
  SAIGE_CHECK(rn->properties.size() == 1);
  SAIGE_CHECK(rn->properties[0].value.int_val == 4);

  // Malformed/corrupted input checks
  EditorScene dummy;
  SAIGE_CHECK(!EditorProjectSerializer::deserialize({}, dummy));
  SAIGE_CHECK(!EditorProjectSerializer::deserialize(std::vector<std::uint8_t>(60, 0), dummy));
  auto bad_blob = blob;
  bad_blob[10] ^= 0xFF;
  SAIGE_CHECK(!EditorProjectSerializer::deserialize(bad_blob, dummy));
}

void test_save_reopen_stable_id_preservation() {
  EditorScene s1(64.0);
  CommandHistory h1;
  EditorNode a{};
  a.name = "RoadSegment_A";
  a.kind = EditorNodeKind::Road;
  a.asset = {"assets/roads/two_lane.saige_asset", 2, 0xABCDULL};
  SAIGE_CHECK(h1.execute(s1, std::make_unique<CreateNodeCommand>(a)));

  const auto blob1 = EditorProjectSerializer::serialize(s1);
  EditorScene s2;
  SAIGE_CHECK(EditorProjectSerializer::deserialize(blob1, s2));
  SAIGE_CHECK(s2.find_node(1) != nullptr);
  SAIGE_CHECK(s2.next_id() == 2);

  CommandHistory h2;
  EditorNode b{};
  b.name = "OakTree_01";
  b.kind = EditorNodeKind::Vegetation;
  b.asset = {"assets/foliage/oak.saige_asset", 1, 0xDEADULL};
  SAIGE_CHECK(h2.execute(s2, std::make_unique<CreateNodeCommand>(b)));
  SAIGE_CHECK(s2.find_node(2) != nullptr);

  const auto blob2 = EditorProjectSerializer::serialize(s2);
  EditorScene s3;
  SAIGE_CHECK(EditorProjectSerializer::deserialize(blob2, s3));
  SAIGE_CHECK(s3.find_node(1)->asset.content_hash == 0xABCDULL);
  SAIGE_CHECK(s3.find_node(2)->asset.content_hash == 0xDEADULL);
}

void test_asset_registry_and_viewport_controls() {
  AssetRegistry reg;
  SAIGE_CHECK(reg.register_asset({{"assets/buildings/diner.saige_asset", 1, 11}, "Diner", AssetCategory::Building, EditorNodeKind::Building, true}));
  SAIGE_CHECK(reg.register_asset({{"assets/foliage/pine.saige_asset", 1, 22}, "Pine Tree", AssetCategory::Vegetation, EditorNodeKind::Vegetation, true}));
  SAIGE_CHECK(!reg.register_asset({{"", 1, 0}, "Invalid", AssetCategory::Prop, EditorNodeKind::Prop, false}));

  SAIGE_CHECK(reg.search(AssetCategory::Building, "din").size() == 1);
  SAIGE_CHECK(reg.search(AssetCategory::All, "pine").size() == 1);

  EditorScene scene;
  EditorNode n{};
  n.id = 1;
  n.name = "Diner_01";
  n.local_transform.position = {40.0, 80.0, 3.0};
  SAIGE_CHECK(scene.insert_node(n));
  SAIGE_CHECK(scene.selection().select(1));

  ViewportState vp{};
  SAIGE_CHECK(vp.focus_on_selection(scene));
  SAIGE_CHECK(std::abs(vp.camera_target.x - 40.0) < 1e-6);
  vp.projection = ViewportProjection::OrthographicTop;
  vp.zoom(-10.0);
  SAIGE_CHECK(std::abs(vp.ortho_width_m - 40.0) < 1e-6);
}

void test_validator_bridge_and_export_gate() {
  class MockEngineValidator final : public IWorldValidatorBridge {
   public:
    bool fail_nav = false;
    [[nodiscard]] std::vector<EngineValidationFinding> validate_scene(
        const EditorScene& scene) const override {
      std::vector<EngineValidationFinding> out;
      if (fail_nav && !scene.nodes().empty()) {
        out.push_back({EngineValidationDomain::Navigation,
                       DiagnosticSeverity::Error, scene.nodes().front().id,
                       "NAV_BLOCKED", "Road blocks pedestrian crossing."});
      }
      return out;
    }
  };

  EditorScene scene;
  EditorNode road{};
  road.id = 1;
  road.name = "MainSt";
  road.kind = EditorNodeKind::Road;
  road.asset.asset_uri = "assets/roads/main.saige_asset";
  SAIGE_CHECK(scene.insert_node(road));

  MockEngineValidator validator;
  validator.fail_nav = true;
  const auto blocked = WorldDataExporter::export_validated_scene(scene, &validator);
  SAIGE_CHECK(!blocked.success);
  SAIGE_CHECK(blocked.blocking_diagnostics.size() == 1);

  validator.fail_nav = false;
  const auto ok = WorldDataExporter::export_validated_scene(scene, &validator);
  SAIGE_CHECK(ok.success);
  SAIGE_CHECK(ok.json_payload.find("right_handed_z_up_meters") != std::string::npos);
}

void test_large_scene_10k_benchmark() {
  EditorScene scene(64.0);
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint32_t i = 1; i <= 10000; ++i) {
    EditorNode n{};
    n.id = i;
    n.name = "Prop";
    n.kind = EditorNodeKind::Prop;
    n.asset.asset_uri = "assets/prop.saige_asset";
    n.local_transform.position = {
        static_cast<double>(i % 100) * 16.0,
        static_cast<double>(i / 100) * 16.0,
        0.0,
    };
    SAIGE_CHECK(scene.insert_node(std::move(n)));
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto q = scene.query_chunk({1, 1});
  const auto t2 = std::chrono::steady_clock::now();
  const auto diags = scene.validate();
  const auto t3 = std::chrono::steady_clock::now();
  const auto blob = EditorProjectSerializer::serialize(scene);
  const auto t4 = std::chrono::steady_clock::now();

  SAIGE_CHECK(diags.empty());
  SAIGE_CHECK(!q.empty());

  const double insert_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double query_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  const double validate_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
  const double serialize_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();

  std::cout << "[editor-10k] insert_ms=" << insert_ms
            << " chunk_query_ms=" << query_ms
            << " validate_ms=" << validate_ms
            << " serialize_ms=" << serialize_ms
            << " blob_kb=" << (blob.size() / 1024) << "\n";
}

}  // namespace

int main() {
  test_hierarchy_reparenting_and_cycle_guard();
  test_snapping_duplication_and_grouping();
  test_transactional_undo_redo_and_rollback();
  test_delete_subtree_undo_and_locked_protection();
  test_scene_validation_diagnostics();
  test_project_serialization_and_corruption_guards();
  test_save_reopen_stable_id_preservation();
  test_asset_registry_and_viewport_controls();
  test_validator_bridge_and_export_gate();
  test_large_scene_10k_benchmark();
  std::cout << "editor_core_test: ALL 10 SUITES PASSED\n";
  return 0;
}
