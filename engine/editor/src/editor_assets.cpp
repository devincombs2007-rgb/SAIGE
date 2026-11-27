#include "saige/editor_assets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace saige::editor {

namespace {

[[nodiscard]] std::string to_lower_ascii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

bool AssetLibrary::register_asset(AssetMetadata meta) {
  if (meta.asset_id == 0 || meta.uri.empty()) return false;
  if (id_to_idx_.count(meta.asset_id) || uri_to_idx_.count(meta.uri)) {
    return false;
  }
  const std::size_t idx = assets_.size();
  id_to_idx_[meta.asset_id] = idx;
  uri_to_idx_[meta.uri] = idx;
  if (meta.content_hash != 0) {
    hash_to_ids_[meta.content_hash].push_back(meta.asset_id);
  }
  assets_.push_back(std::move(meta));
  return true;
}

bool AssetLibrary::remove_asset(std::uint64_t asset_id) {
  auto it = id_to_idx_.find(asset_id);
  if (it == id_to_idx_.end()) return false;
  const std::size_t idx = it->second;
  const std::size_t last = assets_.size() - 1;

  const auto old_hash = assets_[idx].content_hash;
  if (old_hash != 0) {
    auto& vec = hash_to_ids_[old_hash];
    vec.erase(std::remove(vec.begin(), vec.end(), asset_id), vec.end());
  }
  uri_to_idx_.erase(assets_[idx].uri);
  id_to_idx_.erase(it);

  if (idx != last) {
    assets_[idx] = std::move(assets_[last]);
    id_to_idx_[assets_[idx].asset_id] = idx;
    uri_to_idx_[assets_[idx].uri] = idx;
  }
  assets_.pop_back();
  return true;
}

bool AssetLibrary::set_favorite(std::uint64_t asset_id, bool fav) {
  auto it = id_to_idx_.find(asset_id);
  if (it == id_to_idx_.end()) return false;
  assets_[it->second].favorite = fav;
  return true;
}

bool AssetLibrary::increment_usage(const std::string& uri) {
  auto it = uri_to_idx_.find(uri);
  if (it == uri_to_idx_.end()) return false;
  ++assets_[it->second].usage_count;
  return true;
}

const AssetMetadata* AssetLibrary::find_by_id(
    std::uint64_t asset_id) const noexcept {
  auto it = id_to_idx_.find(asset_id);
  return it != id_to_idx_.end() ? &assets_[it->second] : nullptr;
}

const AssetMetadata* AssetLibrary::find_by_uri(
    const std::string& uri) const noexcept {
  auto it = uri_to_idx_.find(uri);
  return it != uri_to_idx_.end() ? &assets_[it->second] : nullptr;
}

std::vector<const AssetMetadata*> AssetLibrary::search(
    std::optional<LibraryAssetCategory> category,
    const std::string& query,
    const std::string& required_tag,
    bool favorites_only) const {
  const std::string q = to_lower_ascii(query);
  const std::string tag_q = to_lower_ascii(required_tag);
  std::vector<const AssetMetadata*> out;
  for (const auto& a : assets_) {
    if (category.has_value() && a.category != *category) continue;
    if (favorites_only && !a.favorite) continue;
    if (!tag_q.empty()) {
      bool has_tag = false;
      for (const auto& t : a.tags) {
        if (to_lower_ascii(t) == tag_q) {
          has_tag = true;
          break;
        }
      }
      if (!has_tag) continue;
    }
    if (!q.empty()) {
      const bool match_name = to_lower_ascii(a.display_name).find(q) != std::string::npos;
      const bool match_uri = to_lower_ascii(a.uri).find(q) != std::string::npos;
      if (!match_name && !match_uri) continue;
    }
    out.push_back(&a);
  }
  return out;
}

AssetValidationReport AssetLibrary::validate_asset(std::uint64_t asset_id) const {
  AssetValidationReport rep{};
  const AssetMetadata* a = find_by_id(asset_id);
  if (!a) {
    rep.passed = false;
    rep.error_count = 1;
    rep.issues.push_back({DiagnosticSeverity::Error, asset_id, "",
                          "ASSET_NOT_FOUND", "Asset ID does not exist in library.",
                          "Register the asset before validating."});
    return rep;
  }

  auto add_issue = [&](DiagnosticSeverity sev, const std::string& code,
                       const std::string& msg, const std::string& sug) {
    rep.issues.push_back({sev, a->asset_id, a->uri, code, msg, sug});
    if (sev == DiagnosticSeverity::Error) {
      ++rep.error_count;
      rep.passed = false;
    } else if (sev == DiagnosticSeverity::Warning) {
      ++rep.warning_count;
    }
  };

  if (a->license_spdx.empty() || a->source_url.empty()) {
    add_issue(DiagnosticSeverity::Error, "MISSING_LICENSE_OR_PROVENANCE",
              "Asset lacks SPDX license identifier or source URL.",
              "Record exact source URL, author, and SPDX license before import.");
  }
  // Provenance (SAIGE, 2026-11-27): Build 012 accepted any non-empty licence string and source URL, an empty author and a self dependency.
  {
    static const char* const kReusable[] = {"CC0-1.0", "CC-BY-4.0", "CC-BY-3.0", "CC-BY-SA-4.0", "MIT", "Apache-2.0", "BSD-2-Clause",
                                            "BSD-3-Clause", "OFL-1.1", "Unlicense", "Zlib", "MIT-0", "0BSD"};
    bool known = false;
    for (const char* id : kReusable) known = known || a->license_spdx == id;
    if (!a->license_spdx.empty() && !known) {
      add_issue(DiagnosticSeverity::Error, "UNRECOGNISED_LICENSE",
                "Licence '" + a->license_spdx + "' is not a recognised reusable SPDX identifier (NC/ND and unknown licences are refused).",
                "Record the exact SPDX identifier of a licence that allows reuse and redistribution.");
    }
    const std::string& u = a->source_url;
    const std::size_t scheme = u.find("://");
    const bool url_ok = (u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0) && scheme != std::string::npos &&
                        u.size() > scheme + 3 && u.find(' ') == std::string::npos && u.find('.', scheme + 3) != std::string::npos;
    if (!u.empty() && !url_ok) {
      add_issue(DiagnosticSeverity::Error, "INVALID_SOURCE_URL", "Source URL '" + u + "' is not an http(s) URL with a host.",
                "Record the page the asset was obtained from.");
    }
    if (a->author.empty()) {
      add_issue(DiagnosticSeverity::Error, "MISSING_AUTHOR", "Asset has no recorded author or creator.",
                "Record the creator named by the source.");
    }
    if (!a->provenance_verified) {
      add_issue(DiagnosticSeverity::Info, "PROVENANCE_UNVERIFIED", "Licence and source are declared, not verified.",
                "Verify the files, the licence text and the source before relying on the declaration.");
    }
  }
  for (const auto& dep : a->dependencies) {
    if (dep == a->uri) {
      add_issue(DiagnosticSeverity::Error, "SELF_DEPENDENCY", "Asset lists itself as a dependency.", "Remove the self reference.");
    }
  }
  if (!a->redistribution_allowed) {
    add_issue(DiagnosticSeverity::Error, "REDISTRIBUTION_DISALLOWED",
              "Asset license does not permit redistribution.",
              "Replace with a CC0 or redistribution-permitted asset.");
  }
  if (!std::isfinite(a->unit_scale_m) || a->unit_scale_m <= 0.0) {
    add_issue(DiagnosticSeverity::Error, "INVALID_SCALE",
              "Asset unit scale must be finite and > 0 meters.",
              "Normalize asset scale to 1 unit = 1 meter.");
  }
  if (!a->z_up_right_handed) {
    add_issue(DiagnosticSeverity::Warning, "NON_ZUP_ORIENTATION",
              "Asset is not right-handed Z-up.",
              "Apply +90 deg X rotation on import to match SAIGE Z-up convention.");
  }
  if (!a->has_normals || !a->has_uvs) {
    add_issue(DiagnosticSeverity::Warning, "MISSING_NORMALS_OR_UVS",
              "Asset is missing vertex normals or UV coordinates.",
              "Re-export with tangents/normals and UV0 enabled.");
  }
  if (!a->has_collision_mesh && a->category != LibraryAssetCategory::Material &&
      a->category != LibraryAssetCategory::Light) {
    add_issue(DiagnosticSeverity::Warning, "MISSING_COLLISION_MESH",
              "Physical asset lacks collision geometry.",
              "Generate convex hull or simplified collision proxy.");
  }
  if (a->lod_count < 2 && a->triangle_count > 15000) {
    add_issue(DiagnosticSeverity::Warning, "MISSING_LODS",
              "High-poly asset has fewer than 2 LODs.",
              "Generate LOD1/LOD2 for streaming distance budgets.");
  }
  for (const auto& dep : a->dependencies) {
    if (!find_by_uri(dep)) {
      add_issue(DiagnosticSeverity::Error, "MISSING_DEPENDENCY",
                "Referenced dependency URI not found: " + dep,
                "Import or relink missing dependency asset.");
    }
  }
  if (a->content_hash != 0) {
    auto it = hash_to_ids_.find(a->content_hash);
    if (it != hash_to_ids_.end() && it->second.size() > 1) {
      add_issue(DiagnosticSeverity::Warning, "DUPLICATE_CONTENT_HASH",
                "Duplicate asset binary content hash detected.",
                "Deduplicate references to the canonical asset URI.");
    }
  }

  // Vehicle attachment validation (catches detached hazard lights / wheels)
  if (a->vehicle_meta.has_value()) {
    const auto& vm = *a->vehicle_meta;
    if (!std::isfinite(vm.wheel_radius_m) || vm.wheel_radius_m <= 0.0 || vm.wheel_radius_m > 2.0) {  // SAIGE: Build 012 accepted -0.3 m
      add_issue(DiagnosticSeverity::Error, "INVALID_WHEEL_RADIUS", "Wheel radius must be in (0, 2] m.", "Measure the wheel and record its radius.");
    }
    if (vm.body_bounds_m.is_valid()) {
      const auto check_points = [&](const std::vector<VehicleAttachmentPoint>& pts, const char* code, const char* what) {
        for (const auto& pt : pts) {
          const bool finite = std::isfinite(pt.local_pos_m.x) && std::isfinite(pt.local_pos_m.y) && std::isfinite(pt.local_pos_m.z) &&
                              std::isfinite(pt.tolerance_m) && pt.tolerance_m >= 0.0;
          if (!finite || !vm.body_bounds_m.contains_with_margin(pt.local_pos_m, pt.tolerance_m)) {
            add_issue(DiagnosticSeverity::Error, code, std::string("Vehicle ") + what + " '" + pt.name + "' is outside the body bounds.",
                      "Move the attachment point onto the vehicle.");
          }
        }
      };
      check_points(vm.seats, "DETACHED_VEHICLE_SEAT", "seat");  // SAIGE: Build 012 checked lights only; a seat 9 m above the roof passed
    }
    if (!vm.body_bounds_m.is_valid()) {
      add_issue(DiagnosticSeverity::Error, "INVALID_VEHICLE_BOUNDS",
                "Vehicle body AABB bounds are inverted.",
                "Recompute vehicle body bounding box.");
    } else {
      for (const auto& lt : vm.lights) {
        if (!vm.body_bounds_m.contains_with_margin(lt.local_pos_m, lt.tolerance_m)) {
          add_issue(DiagnosticSeverity::Error, "DETACHED_VEHICLE_LIGHT",
                    "Vehicle light '" + lt.name + "' is detached from body bounds.",
                    "Snap attachment point onto vehicle chassis surface.");
        }
      }
      for (const auto& wh : vm.wheels) {
        if (!vm.body_bounds_m.contains_with_margin(wh.local_pos_m, wh.tolerance_m + vm.wheel_radius_m)) {
          add_issue(DiagnosticSeverity::Error, "DETACHED_VEHICLE_WHEEL",
                    "Vehicle wheel '" + wh.name + "' is outside expected chassis/wheel-well bounds.",
                    "Align wheel hub coordinate to suspension hardpoint.");
        }
      }
    }
  }
  return rep;
}

AssetValidationReport AssetLibrary::validate_all() const {
  AssetValidationReport combined{};
  for (const auto& a : assets_) {
    const auto r = validate_asset(a.asset_id);
    if (!r.passed) combined.passed = false;
    combined.error_count += r.error_count;
    combined.warning_count += r.warning_count;
    combined.issues.insert(combined.issues.end(), r.issues.begin(), r.issues.end());
  }
  return combined;
}

}  // namespace saige::editor
