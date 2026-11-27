#include "saige/editor_materials.hpp"

#include <cmath>

namespace saige::editor {

MaterialAuthoringAsset MaterialAuthoringAsset::from_preset(
    MaterialPreset preset, const std::string& name) {
  MaterialAuthoringAsset m{};
  m.name = name;
  m.preset = preset;
  switch (preset) {
    case MaterialPreset::AutomotivePaint:
      m.base_color_rgb = {0.55, 0.04, 0.06};
      m.metallic = 0.85;
      m.roughness = 0.22;
      m.clearcoat = 1.0;
      break;
    case MaterialPreset::ClearGlass:
      m.is_glass = true;
      m.base_color_rgb = {0.95, 0.98, 0.99};
      m.roughness = 0.03;
      m.glass = {0.10, 0.92, 1.52, 0.03, 0.008, 0.04, {0.95, 0.98, 0.99},
                 FeatureSupportStatus::PlannedEngineDependency};
      break;
    case MaterialPreset::TintedGlass:
      m.is_glass = true;
      m.base_color_rgb = {0.22, 0.28, 0.30};
      m.roughness = 0.04;
      m.glass = {0.28, 0.65, 1.52, 0.04, 0.010, 0.25, {0.30, 0.38, 0.40},
                 FeatureSupportStatus::PlannedEngineDependency};
      break;
    case MaterialPreset::FrostedGlass:
      m.is_glass = true;
      m.base_color_rgb = {0.88, 0.90, 0.92};
      m.roughness = 0.45;
      m.glass = {0.35, 0.60, 1.50, 0.45, 0.012, 0.12, {0.88, 0.90, 0.92},
                 FeatureSupportStatus::PlannedEngineDependency};
      break;
    case MaterialPreset::ReflectiveArchitecturalGlass:
      m.is_glass = true;
      m.base_color_rgb = {0.42, 0.50, 0.54};
      m.metallic = 0.10;
      m.roughness = 0.02;
      m.glass = {0.22, 0.72, 1.54, 0.02, 0.018, 0.18, {0.65, 0.75, 0.80},
                 FeatureSupportStatus::PlannedEngineDependency};
      break;
    case MaterialPreset::WetAsphalt:
      m.base_color_rgb = {0.06, 0.06, 0.07};
      m.roughness = 0.18;
      m.wetness = 0.85;
      break;
    case MaterialPreset::Concrete:
      m.base_color_rgb = {0.42, 0.41, 0.39};
      m.roughness = 0.85;
      break;
    case MaterialPreset::Brick:
      m.base_color_rgb = {0.46, 0.18, 0.12};
      m.roughness = 0.82;
      break;
    case MaterialPreset::Wood:
      m.base_color_rgb = {0.36, 0.22, 0.12};
      m.roughness = 0.62;
      break;
    case MaterialPreset::Leather:
      m.base_color_rgb = {0.20, 0.12, 0.08};
      m.roughness = 0.48;
      break;
    case MaterialPreset::Fabric:
      m.base_color_rgb = {0.32, 0.34, 0.38};
      m.roughness = 0.90;
      break;
    case MaterialPreset::BrushedMetal:
      m.base_color_rgb = {0.72, 0.74, 0.76};
      m.metallic = 1.0;
      m.roughness = 0.32;
      break;
    case MaterialPreset::Custom:
      break;
  }
  return m;
}

std::vector<ValidationDiagnostic> MaterialAuthoringAsset::validate() const {
  std::vector<ValidationDiagnostic> out;
  auto check_01 = [&](double v, const char* code, const char* msg) {
    if (!std::isfinite(v) || v < 0.0 || v > 1.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, code, msg});
    }
  };
  check_01(metallic, "INVALID_METALLIC", "Metallic must be in [0, 1].");
  check_01(roughness, "INVALID_ROUGHNESS", "Roughness must be in [0, 1].");
  check_01(clearcoat, "INVALID_CLEARCOAT", "Clearcoat must be in [0, 1].");
  check_01(wetness, "INVALID_WETNESS", "Wetness must be in [0, 1].");
  if (!std::isfinite(tiling_uv) || tiling_uv <= 0.0) {
    out.push_back({DiagnosticSeverity::Error, kInvalidEntityId,
                   "INVALID_TILING_UV", "UV tiling must be > 0."});
  }
  if (is_glass) {
    if (!std::isfinite(glass.ior) || glass.ior < 1.0 || glass.ior > 2.6) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId,
                     "INVALID_GLASS_IOR", "Glass IOR must be in [1.0, 2.6]."});
    }
    if (glass.opacity > 0.95 && glass.transmission < 0.05) {
      out.push_back({DiagnosticSeverity::Warning, kInvalidEntityId,
                     "OPAQUE_GLASS_WARNING",
                     "Glass material is nearly opaque with zero transmission."});
    }
  }
  if (interior_mapping.enabled) {
    if (interior_mapping.room_width_m <= 0.5 ||
        interior_mapping.room_height_m <= 0.5 ||
        interior_mapping.room_depth_m <= 0.5) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId,
                     "INVALID_INTERIOR_ROOM_DIMS",
                     "Interior mapping room dimensions must exceed 0.5 m."});
    }
  }
  return out;
}

}  // namespace saige::editor
