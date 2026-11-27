#pragma once

#include "saige/editor_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saige::editor {

enum class FeatureSupportStatus : std::uint8_t {
  EngineSupported = 0,
  PlannedEngineDependency = 1,
};

enum class MaterialPreset : std::uint8_t {
  Custom = 0,
  AutomotivePaint = 1,
  ClearGlass = 2,
  TintedGlass = 3,
  FrostedGlass = 4,
  ReflectiveArchitecturalGlass = 5,
  WetAsphalt = 6,
  Concrete = 7,
  Brick = 8,
  Wood = 9,
  Leather = 10,
  Fabric = 11,
  BrushedMetal = 12,
};

struct GlassParameters {
  double opacity = 0.15;        // [0.02, 1.0] - avoids opaque flat blue
  double transmission = 0.85;   // [0.0, 1.0]
  double ior = 1.52;            // Crown glass ~1.52
  double roughness = 0.04;      // [0.0, 1.0]
  double thickness_m = 0.012;
  double absorption = 0.08;
  Vec3d tint_rgb{0.92, 0.96, 0.98};
  FeatureSupportStatus refraction_status =
      FeatureSupportStatus::PlannedEngineDependency;
};

struct InteriorMappingParameters {
  bool enabled = false;
  double room_width_m = 4.0;
  double room_height_m = 3.0;
  double room_depth_m = 4.5;
  double day_night_blend = 0.0;  // 0 = day, 1 = night lit interior
  std::uint32_t room_seed = 1;
  double distance_fade_m = 120.0;
  FeatureSupportStatus shader_status =
      FeatureSupportStatus::PlannedEngineDependency;
};

struct MaterialAuthoringAsset {
  std::string name;
  MaterialPreset preset = MaterialPreset::Custom;
  Vec3d base_color_rgb{0.5, 0.5, 0.5};
  double metallic = 0.0;
  double roughness = 0.5;
  double clearcoat = 0.0;
  double wetness = 0.0;
  double emissive_nits = 0.0;
  double tiling_uv = 1.0;

  bool is_glass = false;
  GlassParameters glass{};
  InteriorMappingParameters interior_mapping{};

  [[nodiscard]] static MaterialAuthoringAsset from_preset(
      MaterialPreset preset, const std::string& name);
  [[nodiscard]] std::vector<ValidationDiagnostic> validate() const;
};

}  // namespace saige::editor
