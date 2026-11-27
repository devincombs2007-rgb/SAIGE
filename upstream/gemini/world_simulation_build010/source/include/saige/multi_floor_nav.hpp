#pragma once

#include <cstdint>
#include <vector>

namespace saige::multi_floor_nav {

enum class PortalKind : std::uint8_t {
  Stair = 0,
  Ramp = 1,
  Elevator = 2,
  InteriorDoorway = 3,
};

enum class PortalTraversalState : std::uint8_t {
  OnFloor = 0,
  TraversingStair = 1,
  TraversingRamp = 2,
  UsingElevator = 3,
  PassingDoorway = 4,
};

struct NavNodeId {
  std::int32_t gx = 0;
  std::int32_t gy = 0;
  std::uint8_t layer = 0;

  [[nodiscard]] bool operator==(const NavNodeId& other) const noexcept = default;
};

struct FloorSlice {
  NavNodeId id{};
  double z_m = 0.0;
  bool walkable = true;
  bool is_interior = false;
};

struct NavPortalLink {
  std::uint32_t id = 0;
  NavNodeId from{};
  NavNodeId to{};
  PortalKind kind = PortalKind::Stair;
  double traversal_cost = 2.0;
  double traversal_duration_sec = 2.5;
  bool bidirectional = true;
};

// Smooth portal traversal state driving SAIGE's character controller & animation
// without instantaneous Z-elevation snaps.
struct PortalTraversalProgress {
  PortalTraversalState state = PortalTraversalState::OnFloor;
  NavNodeId from_node{};
  NavNodeId to_node{};
  double from_z_m = 0.0;
  double to_z_m = 0.0;
  double duration_sec = 0.0;
  double elapsed_sec = 0.0;

  void begin(PortalKind kind, NavNodeId from, NavNodeId to, double z_from,
             double z_to, double duration_s) noexcept;
  void step(double dt_seconds) noexcept;
  [[nodiscard]] bool is_active() const noexcept;
  [[nodiscard]] bool is_complete() const noexcept;
  [[nodiscard]] double current_z_m() const noexcept;
  [[nodiscard]] double normalized_progress() const noexcept;
};

struct MultiFloorPathResult {
  bool found = false;
  double total_cost = 0.0;
  std::vector<NavNodeId> waypoints;
  std::uint32_t portal_transitions = 0;
  std::uint32_t interior_steps = 0;
};

class MultiFloorNavGraph {
 public:
  explicit MultiFloorNavGraph(double cell_size_m = 0.5,
                              double max_step_height_m = 0.35);

  bool set_slice(const FloorSlice& slice);
  [[nodiscard]] const FloorSlice* find_slice(NavNodeId id) const noexcept;

  bool add_portal(const NavPortalLink& portal);
  [[nodiscard]] const NavPortalLink* find_portal_between(
      NavNodeId a, NavNodeId b) const noexcept;

  [[nodiscard]] MultiFloorPathResult find_path(NavNodeId start,
                                               NavNodeId goal) const;

  [[nodiscard]] std::size_t slice_count() const noexcept { return slices_.size(); }
  [[nodiscard]] std::size_t portal_count() const noexcept { return portals_.size(); }

 private:
  double cell_size_m_ = 0.5;
  double max_step_height_m_ = 0.35;
  std::vector<FloorSlice> slices_;
  std::vector<NavPortalLink> portals_;
};

}  // namespace saige::multi_floor_nav
