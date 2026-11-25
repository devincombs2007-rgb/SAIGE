#pragma once

#include "saige/npc_simulation.hpp"

#include <cmath>
#include <cstdint>

// Compile-time adapter bridging saige::world_sim::WorldPosition to SAIGE's
// native large-world WorldPosition (src/large_world.cpp, commit 96c4878)
// without modifying shared headers. Define SAIGE_HAS_NATIVE_LARGE_WORLD=1
// when compiling inside saige_core with large_world.hpp available.

#if defined(SAIGE_HAS_NATIVE_LARGE_WORLD) && SAIGE_HAS_NATIVE_LARGE_WORLD
#include "saige/large_world.hpp"
#endif

namespace saige::world_sim {

inline constexpr double kSaigeNativeChunkSizeM = 64.0;

template <typename NativeWorldPosT>
[[nodiscard]] inline WorldPosition from_native_world_position(
    const NativeWorldPosT& native_pos,
    double native_chunk_size_m = kSaigeNativeChunkSizeM,
    double sim_chunk_size_m = kDefaultChunkSizeM) noexcept {
  if (std::abs(native_chunk_size_m - sim_chunk_size_m) <= 1e-9) {
    WorldPosition out{};
    out.cell.x = native_pos.cell.x;
    out.cell.y = native_pos.cell.y;
    out.local.x = native_pos.local.x;
    out.local.y = native_pos.local.y;
    out.local.z = native_pos.local.z;
    return out;
  }
  const Vec3d world_m{
      static_cast<double>(native_pos.cell.x) * native_chunk_size_m +
          static_cast<double>(native_pos.local.x),
      static_cast<double>(native_pos.cell.y) * native_chunk_size_m +
          static_cast<double>(native_pos.local.y),
      static_cast<double>(native_pos.local.z),
  };
  return WorldPosition::from_world_double(world_m, sim_chunk_size_m);
}

template <typename NativeWorldPosT>
[[nodiscard]] inline NativeWorldPosT to_native_world_position(
    const WorldPosition& pos,
    double sim_chunk_size_m = kDefaultChunkSizeM,
    double native_chunk_size_m = kSaigeNativeChunkSizeM) noexcept {
  NativeWorldPosT out{};
  if (std::abs(native_chunk_size_m - sim_chunk_size_m) <= 1e-9) {
    out.cell.x = pos.cell.x;
    out.cell.y = pos.cell.y;
    out.local.x = pos.local.x;
    out.local.y = pos.local.y;
    out.local.z = pos.local.z;
    return out;
  }
  const Vec3d world_m = pos.to_world_double(sim_chunk_size_m);
  const double cx = std::floor(world_m.x / native_chunk_size_m);
  const double cy = std::floor(world_m.y / native_chunk_size_m);
  out.cell.x = static_cast<decltype(out.cell.x)>(cx);
  out.cell.y = static_cast<decltype(out.cell.y)>(cy);
  out.local.x = static_cast<float>(world_m.x - cx * native_chunk_size_m);
  out.local.y = static_cast<float>(world_m.y - cy * native_chunk_size_m);
  out.local.z = static_cast<float>(world_m.z);
  return out;
}

template <typename NativeWorldPosT>
[[nodiscard]] inline bool validate_native_roundtrip(
    const NativeWorldPosT& native_pos, double tolerance_m = 1e-3,
    double native_chunk_size_m = kSaigeNativeChunkSizeM) noexcept {
  const WorldPosition sim_p =
      from_native_world_position(native_pos, native_chunk_size_m);
  const NativeWorldPosT back =
      to_native_world_position<NativeWorldPosT>(sim_p, kDefaultChunkSizeM,
                                                native_chunk_size_m);
  const double dx = (static_cast<double>(back.cell.x) -
                     static_cast<double>(native_pos.cell.x)) *
                        native_chunk_size_m +
                    (static_cast<double>(back.local.x) -
                     static_cast<double>(native_pos.local.x));
  const double dy = (static_cast<double>(back.cell.y) -
                     static_cast<double>(native_pos.cell.y)) *
                        native_chunk_size_m +
                    (static_cast<double>(back.local.y) -
                     static_cast<double>(native_pos.local.y));
  const double dz =
      static_cast<double>(back.local.z) - static_cast<double>(native_pos.local.z);
  return (dx * dx + dy * dy + dz * dz) <= (tolerance_m * tolerance_m);
}

}  // namespace saige::world_sim
