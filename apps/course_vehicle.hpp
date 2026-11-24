#pragma once

#include "course_input.hpp"
#include "saige/capsule_character.hpp"
#include "saige/wheel_contact.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>

namespace saige::sample {

struct CourseVehiclePose {
    Vec3 position;
    float heading;
};

class CourseVehicle {
public:
    explicit CourseVehicle(Vec3 spawn) : spawn_(spawn), position_(spawn) {}

    void step(MovementActions input, bool brake, std::chrono::nanoseconds step, std::span<const Aabb> boxes,
              std::span<const Triangle> triangles = {}) {
        const float dt = std::chrono::duration<float>(step).count();
        const float target = brake ? 0.0f : input.forward * (input.forward >= 0 ? 10.0f : 4.0f);
        const float acceleration = brake ? 14.0f : input.forward == 0 ? 3.0f : 6.0f;
        speed_ += std::clamp(target - speed_, -dt * acceleration, dt * acceleration);
        const float steering = input.left * std::clamp(speed_ * 0.4f, -1.0f, 1.0f);
        heading_ = std::remainder(heading_ + steering * 1.3f * dt, 2.0f * std::numbers::pi_v<float>);
        const Vec3 move{std::cos(heading_) * speed_ * dt, std::sin(heading_) * speed_ * dt, 0};
        const Capsule body{position_, radius, 2.0f * radius};
        Vec3 candidate{position_.x + move.x, position_.y + move.y, position_.z};
        if (const auto hit = sweep(body, move, boxes, triangles)) {
            candidate = hit->stop;
            park();
        }
        // This prototype stays on its flat driving surface.
        if (!can_occupy(candidate, boxes, triangles)) {
            park();
            return;
        }
        position_ = candidate;
    }

    // Opt-in fixed-step course handling. Yaw is conservatively checked at its candidate orientation.
    void step_terrain(MovementActions input, bool brake, std::chrono::nanoseconds step,
                      const StaticCollision& world, QueryScratch& scratch) {
        const auto current = support(position_, heading_, world, scratch);
        if (!current) { park(); return; }
        const float dt = std::chrono::duration<float>(step).count();
        const float target = brake ? 0.0f : input.forward * (input.forward >= 0 ? 10.0f : 4.0f);
        const float traction = 0.25f * static_cast<float>(current->wheels) * current->normal.z;
        const float limit = (brake ? 14.0f : input.forward == 0 ? 3.0f : 6.0f) * traction * dt;
        float speed = speed_ + std::clamp(target - speed_, -limit, limit);
        if (input.forward != 0 && !brake) {
            const float descent = current->normal.x * std::cos(heading_) + current->normal.y * std::sin(heading_);
            speed = std::clamp(speed + descent * 3.0f * dt, -4.0f, 10.0f);
        }
        const float steering = input.left * std::clamp(speed * 0.4f, -1.0f, 1.0f);
        float heading = std::remainder(heading_ + steering * 1.3f * dt, 2.0f * std::numbers::pi_v<float>);
        if (world.overlap_oriented_box(chassis(position_, heading), scratch)) heading = heading_;

        const float dx = std::cos(heading) * speed * dt;
        const float dy = std::sin(heading) * speed * dt;
        Vec3 next{position_.x + dx, position_.y + dy, position_.z};
        const auto target_support = support(next, heading, world, scratch);
        if (!target_support) { park(); return; }
        const float height_delta = std::clamp(target_support->height - position_.z, -4.0f * dt, 6.0f * dt);
        next.z += height_delta;
        if (const auto hit = world.sweep_oriented_box(chassis(position_, heading), {dx, dy, height_delta}, scratch)) {
            if (!hit->hit.stop_clear) { park(); return; }
            next = hit->hit.stop;
            next.z -= chassis_lift;
            speed = 0;
        }
        if (!can_occupy_terrain(next, heading, world, scratch)) { park(); return; }
        position_ = next;
        heading_ = heading;
        speed_ = speed;
    }

    [[nodiscard]] static bool can_occupy_terrain(Vec3 position, float heading, const StaticCollision& world,
                                                 QueryScratch& scratch) {
        const auto floor = support(position, heading, world, scratch);
        return floor && std::abs(floor->height - position.z) <= 0.24f &&
               !world.overlap_oriented_box(chassis(position, heading), scratch);
    }

    [[nodiscard]] bool can_enter(const CapsuleCharacterState& character, const CapsuleCharacterConfig& config,
                                 std::span<const Aabb> boxes, std::span<const Triangle> triangles = {}) const {
        const Vec3 approach{position_.x - character.feet.x, position_.y - character.feet.y, 0};
        return character.grounded && std::abs(character.feet.z - position_.z) < 0.25f &&
               std::hypot(approach.x, approach.y) <= 1.4f &&
               !sweep({character.feet, config.radius, config.height}, approach, boxes, triangles);
    }

    [[nodiscard]] std::optional<Vec3> exit_point(const CapsuleCharacterConfig& config,
                                                 std::span<const Aabb> boxes,
                                                 std::span<const Triangle> triangles = {}) const {
        if (std::abs(speed_) > 0.25f) return {};
        const Capsule rider{position_, config.radius, config.height};
        const Vec3 left{-std::sin(heading_), std::cos(heading_), 0};
        for (const float distance : {1.15f, -1.15f}) {
            const Vec3 move{left.x * distance, left.y * distance, 0};
            if (sweep(rider, move, boxes, triangles)) continue;
            const Capsule side{{position_.x + move.x, position_.y + move.y, position_.z},
                               rider.radius, rider.height};
            if (const auto floor = floor_below(side, boxes, triangles)) return floor->stop;
        }
        return {};
    }

    [[nodiscard]] std::optional<Vec3> exit_point_terrain(const CapsuleCharacterConfig& config,
                                                          const StaticCollision& world, QueryScratch& scratch) const {
        if (std::abs(speed_) > 0.25f) return {};
        constexpr float clearance = 0.28f;
        constexpr float reach = 0.65f;
        const Capsule raised{{position_.x, position_.y, position_.z + clearance}, config.radius, config.height};
        if (world.sweep_capsule({position_, config.radius, config.height}, {0, 0, clearance}, scratch)) return {};
        const Vec3 left{-std::sin(heading_), std::cos(heading_), 0};
        for (const float distance : {1.15f, -1.15f}) {
            const Vec3 lateral{left.x * distance, left.y * distance, 0};
            if (world.sweep_capsule(raised, lateral, scratch)) continue;
            const Capsule side{{raised.feet.x + lateral.x, raised.feet.y + lateral.y, raised.feet.z},
                               raised.radius, raised.height};
            const auto ground = world.sweep_capsule(side, {0, 0, -reach}, scratch);
            if (ground && !ground->hit.started_overlapping && ground->hit.normal.z >= std::cos(config.max_slope_radians)) {
                return ground->hit.stop;
            }
        }
        return {};
    }

    [[nodiscard]] bool can_occupy(Vec3 feet, std::span<const Aabb> boxes,
                                  std::span<const Triangle> triangles = {}) const {
        const Capsule body{feet, radius, 2.0f * radius};
        return std::ranges::none_of(boxes, [&](const auto& box) { return capsule_box_push_out(body, box).has_value(); }) &&
               std::ranges::none_of(triangles, [&](const auto& triangle) { return capsule_triangle_push_out(body, triangle).has_value(); }) &&
               floor_below(body, boxes, triangles).has_value();
    }

    void restore_parked(CourseVehiclePose pose) noexcept {
        position_ = pose.position;
        heading_ = pose.heading;
        park();
    }

    void migrate_origin(Vec3 shift) noexcept {
        position_ = {position_.x + shift.x, position_.y + shift.y, position_.z + shift.z};
        spawn_ = {spawn_.x + shift.x, spawn_.y + shift.y, spawn_.z + shift.z};
    }

    void park() noexcept { speed_ = 0; }
    void reset() noexcept {
        position_ = spawn_;
        heading_ = 0;
        park();
    }
    [[nodiscard]] Vec3 position() const noexcept { return position_; }
    [[nodiscard]] float heading() const noexcept { return heading_; }
    [[nodiscard]] float speed() const noexcept { return speed_; }

private:
    struct TerrainSupport {
        float height;
        Vec3 normal;
        int wheels;
    };

    static constexpr float chassis_lift = 0.5f;
    [[nodiscard]] static OrientedBox chassis(Vec3 position, float heading) {
        position.z += chassis_lift;
        return make_oriented_box(position, {0.62f, 0.53f, 0.25f}, heading);
    }

    [[nodiscard]] static std::optional<TerrainSupport> support(Vec3 position, float heading, const StaticCollision& world,
                                                                QueryScratch& scratch) {
        constexpr std::array wheel_mounts{Vec3{0.44f, 0.36f, 0}, Vec3{0.44f, -0.36f, 0},
                                          Vec3{-0.44f, 0.36f, 0}, Vec3{-0.44f, -0.36f, 0}};
        const float cos_yaw = std::cos(heading), sin_yaw = std::sin(heading);
        TerrainSupport terrain{position.z - 0.41f, {}, 0};
        for (const auto mount : wheel_mounts) {
            WheelProbe probe;
            probe.origin = {position.x + cos_yaw * mount.x - sin_yaw * mount.y,
                            position.y + sin_yaw * mount.x + cos_yaw * mount.y, position.z + 0.46f};
            probe.length = 0.87f;
            probe.max_slope_radians = 0.52f;
            const auto contact = probe_wheel(world, probe, scratch);
            if (contact.status == WheelContactStatus::Blocked || contact.status == WheelContactStatus::InitialOverlap) return {};
            if (contact.status == WheelContactStatus::Support) {
                terrain.height = std::max(terrain.height, contact.point.z);
                terrain.normal.x += contact.normal.x;
                terrain.normal.y += contact.normal.y;
                terrain.normal.z += contact.normal.z;
                ++terrain.wheels;
            }
        }
        if (terrain.wheels < 3) return {};
        const float length = std::hypot(terrain.normal.x, terrain.normal.y, terrain.normal.z);
        terrain.normal = {terrain.normal.x / length, terrain.normal.y / length, terrain.normal.z / length};
        return terrain;
    }

    [[nodiscard]] static std::optional<CapsuleHit> sweep(Capsule capsule, Vec3 movement,
                                                         std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
        const auto box = sweep_capsule_boxes(capsule, movement, boxes);
        const auto triangle = sweep_capsule_triangles(capsule, movement, triangles);
        return triangle && (!box || triangle->contact_t < box->contact_t) ? triangle : box;
    }

    [[nodiscard]] static std::optional<CapsuleHit> floor_below(Capsule capsule, std::span<const Aabb> boxes,
                                                               std::span<const Triangle> triangles) {
        const auto hit = sweep(capsule, {0, 0, -0.08f}, boxes, triangles);
        return hit && !hit->started_overlapping && hit->normal.z > 0.99f ? hit : std::nullopt;
    }

    static constexpr float radius = 0.6f;
    Vec3 spawn_;
    Vec3 position_;
    float heading_{};
    float speed_{};
};

} // namespace saige::sample
