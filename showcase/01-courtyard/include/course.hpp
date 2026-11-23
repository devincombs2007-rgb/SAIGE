#pragma once

// Showcase 01 — the scripted course: scene data (colliders, decals, route) and the driver that walks the route with the real
// CapsuleCharacter. No GPU, no clock: the same inputs give the same trace, tick for tick (checked by the showcase_course test).

#include "saige/capsule_character.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace saige::showcase {

struct Decal {
    float x{}, y{}, z{};
    float width{}, depth{};  // full extents in meters
};

struct Waypoint {
    float x{}, y{};
    bool sprint = false;
    float speed = 1.0f;           // input magnitude in (0, 1]
    float jump_distance = -1.0f;  // jump once when the horizontal distance falls below this; < 0: never
    float max_seconds = 20.0f;    // give up on the waypoint after this long
    bool timed = false;           // reaching max_seconds is intended (e.g. sliding along a wall); not reported as a failure
};

struct SceneData {
    Vec3 spawn;
    float radius = 0.3f;
    float height = 1.7f;
    std::vector<Aabb> colliders;
    std::vector<std::string> collider_names;
    std::vector<Triangle> triangles;  // ramps and other non-box collision
    std::vector<std::string> triangle_names;
    std::vector<Decal> decals;
    std::vector<Waypoint> route;
};

/// Reads scene.json (written by generate_assets.py). Throws std::runtime_error with the reason.
[[nodiscard]] SceneData load_scene(const std::filesystem::path& scene_json);

enum class EventKind : uint8_t { Jump, Land, StepUp, Blocked, WaypointReached, WaypointTimedOut, WaypointFailed };
[[nodiscard]] const char* to_string(EventKind kind) noexcept;

struct Event {
    uint32_t tick{};
    EventKind kind{};
    uint32_t waypoint{};
};

struct TickRecord {
    Vec3 feet;
    Vec3 velocity;
    bool grounded{};
    float ground_normal_z = 1.0f;  // 1 on level ground; smaller on a slope (meaningful while grounded)
    uint32_t waypoint{};
};

struct CourseResult {
    std::vector<TickRecord> trace;  // one record per fixed tick, after the step
    std::vector<Event> events;
    bool finished = false;          // every waypoint was left behind
    uint32_t failed_waypoints = 0;  // abandoned on timeout without `timed`
    uint64_t hash = 0;              // FNV-1a over the bit patterns of every state
    float max_height = 0.0f;
    float distance = 0.0f;          // horizontal path length
};

inline constexpr std::chrono::nanoseconds fixed_step{16'666'667};  // 60 Hz, matches Core's default step

/// Runs the whole route (or `max_ticks`, whichever ends first) through a CapsuleCharacter built from `scene`.
[[nodiscard]] CourseResult run_course(const SceneData& scene, uint32_t max_ticks = 60 * 120);

/// Top of the highest surface (a box top or a triangle) under (x, y) at or below `z_limit`; 0 if none. Used for the character's blob shadow.
[[nodiscard]] float surface_height(const SceneData& scene, float x, float y, float z_limit);

} // namespace saige::showcase
