#include "saige/editor_workspace.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace saige::editor {

namespace {

[[nodiscard]] std::string to_lower(const std::string& s) {
  std::string out = s;
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

[[nodiscard]] std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    out.push_back(ch);
  }
  return out;
}

}  // namespace

bool AssetRegistry::register_asset(AssetCatalogEntry entry) {
  if (entry.ref.asset_uri.empty()) return false;
  for (const auto& e : entries_) {
    if (e.ref.asset_uri == entry.ref.asset_uri) return false;
  }
  entries_.push_back(std::move(entry));
  return true;
}

std::vector<AssetCatalogEntry> AssetRegistry::search(
    AssetCategory category, const std::string& query) const {
  std::vector<AssetCatalogEntry> out;
  const std::string q = to_lower(query);
  for (const auto& e : entries_) {
    if (category != AssetCategory::All && e.category != category) continue;
    if (!q.empty()) {
      const std::string name_l = to_lower(e.display_name);
      const std::string uri_l = to_lower(e.ref.asset_uri);
      if (name_l.find(q) == std::string::npos &&
          uri_l.find(q) == std::string::npos) {
        continue;
      }
    }
    out.push_back(e);
  }
  return out;
}

const AssetCatalogEntry* AssetRegistry::find_by_uri(
    const std::string& uri) const noexcept {
  for (const auto& e : entries_) {
    if (e.ref.asset_uri == uri) return &e;
  }
  return nullptr;
}

void ViewportState::pan(double dx, double dy, double dz) noexcept {
  if (std::isfinite(dx)) camera_target.x += dx;
  if (std::isfinite(dy)) camera_target.y += dy;
  if (std::isfinite(dz)) camera_target.z += dz;
}

void ViewportState::zoom(double delta_m) noexcept {
  if (!std::isfinite(delta_m)) return;
  if (projection == ViewportProjection::Perspective) {
    orbit_distance_m = std::clamp(orbit_distance_m + delta_m, 1.0, 2000.0);
  } else {
    ortho_width_m = std::clamp(ortho_width_m + delta_m, 2.0, 4000.0);
  }
}

bool ViewportState::focus_on_selection(const EditorScene& scene) noexcept {
  const auto& ids = scene.selection().ids();
  if (ids.empty()) return false;
  Vec3d sum{};
  std::size_t count = 0;
  for (EntityId id : ids) {
    if (const EditorNode* n = scene.find_node(id)) {
      sum.x += n->local_transform.position.x;
      sum.y += n->local_transform.position.y;
      sum.z += n->local_transform.position.z;
      ++count;
    }
  }
  if (count == 0) return false;
  const double inv = 1.0 / static_cast<double>(count);
  camera_target = {sum.x * inv, sum.y * inv, sum.z * inv};
  return true;
}

WorldExportResult WorldDataExporter::export_validated_scene(
    const EditorScene& scene, const IWorldValidatorBridge* engine_validator) {
  WorldExportResult res{};
  const auto local_diags = scene.validate();
  for (const auto& d : local_diags) {
    if (d.severity == DiagnosticSeverity::Error) {
      res.blocking_diagnostics.push_back(d);
    }
  }
  if (engine_validator != nullptr) {
    res.engine_findings = engine_validator->validate_scene(scene);
    for (const auto& f : res.engine_findings) {
      if (f.severity == DiagnosticSeverity::Error) {
        res.blocking_diagnostics.push_back(
            {f.severity, f.node_id, f.code, f.message});
      }
    }
  }
  if (!res.blocking_diagnostics.empty()) {
    res.success = false;
    return res;
  }

  std::ostringstream os;
  os << "{\n"
     << "  \"format\": \"saige_world_export\",\n"
     << "  \"version\": 1,\n"
     << "  \"coordinate_system\": \"right_handed_z_up_meters\",\n"
     << "  \"chunk_size_m\": " << scene.chunk_size_m() << ",\n"
     << "  \"nodes\": [\n";
  for (std::size_t i = 0; i < scene.nodes().size(); ++i) {
    const auto& n = scene.nodes()[i];
    os << "    {\"id\": " << n.id
       << ", \"parent_id\": " << n.parent_id
       << ", \"name\": \"" << escape_json(n.name) << "\""
       << ", \"kind\": " << static_cast<int>(n.kind)
       << ", \"asset_uri\": \"" << escape_json(n.asset.asset_uri) << "\""
       << ", \"asset_version\": " << n.asset.asset_version
       << ", \"chunk\": [" << n.chunk.x << ", " << n.chunk.y << "]"
       << ", \"pos\": [" << n.local_transform.position.x << ", "
       << n.local_transform.position.y << ", " << n.local_transform.position.z
       << "]}" << (i + 1 < scene.nodes().size() ? "," : "") << "\n";
  }
  os << "  ]\n}\n";

  res.success = true;
  res.json_payload = os.str();
  return res;
}

}  // namespace saige::editor
