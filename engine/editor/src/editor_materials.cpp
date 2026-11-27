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
  // SAIGE (2026-11-27): Build 012 range-checked only metallic, roughness, clearcoat, wetness, tiling and the IOR; opacity 5, transmission -1 and a
  // negative thickness produced no error, and NaN room sizes passed because NaN <= 0.5 is false.
  const auto check_rgb01 = [&](const Vec3d& c, const char* code, const char* msg) {
    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || c.x < 0.0 || c.y < 0.0 || c.z < 0.0 || c.x > 1.0 || c.y > 1.0 || c.z > 1.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, code, msg});
    }
  };
  check_rgb01(base_color_rgb, "INVALID_BASE_COLOR", "Base colour components must be in [0, 1].");
  if (!std::isfinite(emissive_nits) || emissive_nits < 0.0) {
    out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, "INVALID_EMISSIVE", "Emissive luminance must be finite and >= 0."});
  }
  if (is_glass) {
    check_01(glass.opacity, "INVALID_GLASS_OPACITY", "Glass opacity must be in [0, 1].");
    check_01(glass.transmission, "INVALID_GLASS_TRANSMISSION", "Glass transmission must be in [0, 1].");
    check_01(glass.roughness, "INVALID_GLASS_ROUGHNESS", "Glass roughness must be in [0, 1].");
    check_rgb01(glass.tint_rgb, "INVALID_GLASS_TINT", "Glass tint components must be in [0, 1].");
    if (!std::isfinite(glass.thickness_m) || glass.thickness_m <= 0.0 || glass.thickness_m > 1.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, "INVALID_GLASS_THICKNESS", "Glass thickness must be in (0, 1] m."});
    }
    if (!std::isfinite(glass.absorption) || glass.absorption < 0.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, "INVALID_GLASS_ABSORPTION", "Glass absorption must be finite and >= 0."});
    }
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
    const auto bad_dim = [](double v) { return !std::isfinite(v) || v <= 0.5 || v > 100.0; };
    if (bad_dim(interior_mapping.room_width_m) ||
        bad_dim(interior_mapping.room_height_m) ||
        bad_dim(interior_mapping.room_depth_m)) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId,
                     "INVALID_INTERIOR_ROOM_DIMS",
                     "Interior mapping room dimensions must be finite, above 0.5 m and at most 100 m."});
    }
    if (!std::isfinite(interior_mapping.day_night_blend) || interior_mapping.day_night_blend < 0.0 || interior_mapping.day_night_blend > 1.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, "INVALID_INTERIOR_BLEND", "Interior day/night blend must be in [0, 1]."});
    }
    if (!std::isfinite(interior_mapping.distance_fade_m) || interior_mapping.distance_fade_m <= 0.0) {
      out.push_back({DiagnosticSeverity::Error, kInvalidEntityId, "INVALID_INTERIOR_FADE", "Interior fade distance must be finite and > 0."});
    }
  }
  return out;
}

}  // namespace saige::editor
