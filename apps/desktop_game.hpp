#pragma once

#include "course_vehicle.hpp"
#include "saige/capsule_character.hpp"
#include "saige/desktop.hpp"
#include "saige/free_camera.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace saige::sample {

class DesktopGame final : public IGame {
public:
    DesktopGame(World& world, IDesktopPlatform& platform, bool course,
                std::optional<std::span<const Aabb>> course_boxes = {}, CapsuleCharacterConfig config = {}, bool enable_vehicle = false, bool terrain_vehicle = false)
        : marker_(world.create()), platform_(platform), terrain_vehicle_(terrain_vehicle) {
        if (!course) return;
        constexpr std::array default_boxes{
            Aabb{{-32, -32, -1}, {32, 32, 0}},
            Aabb{{2.5f, -0.5f, 0}, {3.5f, 0.5f, 1}},
            Aabb{{4.5f, 0.5f, 0}, {5.5f, 1.5f, 1}},
            Aabb{{5.5f, -1.5f, 0}, {6.5f, -0.5f, 1}},
        };
        const auto geometry = course_boxes.value_or(std::span<const Aabb>(default_boxes));
        if (geometry.empty() || geometry.front().min.x >= 0 || geometry.front().max.x <= 0 ||
            geometry.front().min.y >= 0 || geometry.front().max.y <= 0 || geometry.front().max.z != 0) {
            throw std::invalid_argument("The first course box must be a floor with the origin inside its top face");
        }
        configure_course({}, geometry, config);
        for (const auto& box : geometry.subspan(1)) {
            (void)world.create({std::midpoint(box.min.x, box.max.x), std::midpoint(box.min.y, box.max.y), box.min.z});
        }
        if (enable_vehicle) spawn_vehicle(world, {0, -3, 0});
    }

    DesktopGame(World& world, IDesktopPlatform& platform, CapsuleCharacter controller,
                std::span<const Aabb> boxes, std::span<const Triangle> triangles,
                bool enable_vehicle, bool terrain_vehicle = false)
        : marker_(world.create(controller.state().feet)), platform_(platform), motor_(std::move(controller)),
          collision_(boxes, triangles), terrain_vehicle_(terrain_vehicle) {
        if (enable_vehicle) {
            const auto checkpoint = motor_->state().feet;
            const float offset = checkpoint.y >= 4 ? -3.0f : 3.0f;
            spawn_vehicle(world, {checkpoint.x, checkpoint.y + offset, checkpoint.z});
        }
        camera_distance_ = clear_camera_distance();
    }

    [[nodiscard]] Vec3 course_position() const { return driving_ ? vehicle_->position() : motor_->state().feet; }
    [[nodiscard]] const CapsuleCharacterConfig& course_config() const noexcept { return motor_->config(); }
    [[nodiscard]] bool grounded_on_foot() const noexcept { return !driving_ && motor_->state().grounded; }
    void remember_grounded_checkpoint() noexcept { motor_->remember_grounded_checkpoint(); }

    [[nodiscard]] std::optional<CourseVehiclePose> vehicle_pose() const {
        if (!vehicle_) return {};
        return CourseVehiclePose{vehicle_->position(), vehicle_->heading()};
    }

    void prepare_streamed_reset() noexcept {
        driving_ = false;
        if (vehicle_spawn_) vehicle_.emplace(*vehicle_spawn_);
        else if (vehicle_) vehicle_->park();
    }

    bool start_persistent_vehicle(World& world, const std::optional<CourseVehiclePose>& pose) {
        vehicle_marker_ = world.create();
        if (pose) {
            CourseVehicle candidate(pose->position);
            if (can_place_vehicle({pose->position, pose->heading}, collision_)) {
                candidate.restore_parked(*pose);
                vehicle_ = candidate;
                (void)world.set_position(vehicle_marker_, pose->position);
                return true;
            }
        }
        (void)world.set_visible(vehicle_marker_, false);
        return false;
    }

    bool replace_course_collision(std::span<const Aabb> boxes, std::span<const Triangle> triangles,
                                  Vec3 shift, bool detach_vehicle, std::optional<CourseVehiclePose> restore_vehicle) {
        StaticCollision candidate_collision(boxes, triangles);
        auto candidate_vehicle = vehicle_;
        auto candidate_spawn = vehicle_spawn_;
        if (candidate_spawn) {
            candidate_spawn = Vec3{candidate_spawn->x + shift.x, candidate_spawn->y + shift.y, candidate_spawn->z + shift.z};
            if (!can_place_vehicle({*candidate_spawn, 0}, candidate_collision)) return false;
        }
        if (detach_vehicle) {
            if (driving_) return false;
            candidate_vehicle.reset();
        } else if (restore_vehicle) {
            candidate_vehicle.emplace(candidate_spawn.value_or(restore_vehicle->position));
            candidate_vehicle->restore_parked(*restore_vehicle);
        } else if (candidate_vehicle) {
            candidate_vehicle->migrate_origin(shift);
        }
        if (candidate_vehicle && !can_place_vehicle({candidate_vehicle->position(), candidate_vehicle->heading()}, candidate_collision)) return false;
        const bool applied = shift == Vec3{} ? motor_->replace_static_geometry(boxes, triangles)
                                             : motor_->migrate_origin(shift, boxes, triangles);
        if (!applied) return false;
        vehicle_ = candidate_vehicle;
        vehicle_spawn_ = candidate_spawn;
        static_assert(std::is_nothrow_move_assignable_v<StaticCollision>);
        collision_ = std::move(candidate_collision);
        return true;
    }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        if (motor_) {
            const auto actions = read_course_actions(platform_);
            const bool interact = actions.interact && !interact_held_;
            interact_held_ = actions.interact;
            if (!actions.jump) suppress_jump_until_release_ = false;
            if (actions.reset) {
                reset_course(actions.jump);
            } else {
                orbit_.update({.look_x_pixels = actions.look.x_pixels, .look_y_pixels = actions.look.y_pixels}, step);
                orbit_.pitch_radians = std::clamp(orbit_.pitch_radians, -1.25f, -0.16f);
                if (vehicle_ && interact) interact_with_vehicle(actions.jump);
                if (driving_) {
                    if (terrain_vehicle_) vehicle_->step_terrain(actions.movement, actions.jump, step, collision_, vehicle_scratch_);
                    else vehicle_->step(actions.movement, actions.jump, step, collision_.boxes(), collision_.triangles());
                } else {
                    step_character(actions, step);
                }
            }
            update_camera_distance(step);
            if (vehicle_marker_.generation != 0) {
                (void)world.set_visible(vehicle_marker_, vehicle_.has_value());
                if (vehicle_) (void)world.set_position(vehicle_marker_, vehicle_->position());
            }
            if (driving_) {
                auto seated = vehicle_->position();
                seated.z += 0.65f;
                (void)world.set_position(marker_, seated);
            } else {
                (void)world.set_position(marker_, motor_->state().feet);
            }
            return;
        }
        const auto movement = read_movement(platform_);
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * movement.forward);
        position_.y += seconds * 4.0f * movement.left;
        world.set_position(marker_, position_);
    }

    [[nodiscard]] bool driving() const noexcept { return driving_; }

    [[nodiscard]] std::optional<CameraView> camera_view() const override {
        if (!motor_) return {};
        const auto direction = orbit_direction();
        const auto target = orbit_target();
        auto view = orbit_.view();
        view.position = {target.x - direction.x * camera_distance_, target.y - direction.y * camera_distance_,
                         target.z - direction.z * camera_distance_};
        return view;
    }

private:
    bool can_place_vehicle(CourseVehiclePose pose, const StaticCollision& collision) {
        if (terrain_vehicle_) return CourseVehicle::can_occupy_terrain(pose.position, pose.heading, collision, vehicle_scratch_);
        return CourseVehicle(pose.position).can_occupy(pose.position, collision.boxes(), collision.triangles());
    }

    void interact_with_vehicle(bool jump_held) {
        if (driving_) {
            const auto exit = terrain_vehicle_ ? vehicle_->exit_point_terrain(motor_->config(), collision_, vehicle_scratch_)
                                               : vehicle_->exit_point(motor_->config(), collision_.boxes(), collision_.triangles());
            if (!exit || !motor_->teleport(*exit)) return;
            vehicle_->park();
            driving_ = false;
            suppress_jump_until_release_ = jump_held;
        } else if (vehicle_->can_enter(motor_->state(), motor_->config(), collision_.boxes(), collision_.triangles())) {
            // The capsule is inactive while seated. Park its state at the pinned reset point until exit.
            motor_->reset();
            driving_ = true;
        }
    }

    void step_character(const CourseActions& actions, std::chrono::nanoseconds step) {
        const auto [forward, left] = actions.movement;
        const float magnitude = std::max(1.0f, std::hypot(forward, left));
        const float cos_yaw = std::cos(orbit_.yaw_radians);
        const float sin_yaw = std::sin(orbit_.yaw_radians);
        motor_->step({(forward * cos_yaw - left * sin_yaw) / magnitude,
                      (forward * sin_yaw + left * cos_yaw) / magnitude,
                      actions.jump && !suppress_jump_until_release_, actions.sprint}, step);
        if (motor_->state().feet.z < -8.0f) reset_course(actions.jump);
    }

    void spawn_vehicle(World& world, Vec3 spawn) {
        vehicle_.emplace(spawn);
        if (!can_place_vehicle({spawn, 0}, collision_)) {
            throw std::invalid_argument("Vehicle spawn needs clear, supported collision in the reset chunk");
        }
        vehicle_spawn_ = spawn;
        vehicle_marker_ = world.create(spawn);
    }

    void configure_course(Vec3 spawn, std::span<const Aabb> geometry, const CapsuleCharacterConfig& config) {
        motor_.emplace(spawn, geometry, config);
        collision_ = StaticCollision(geometry, {});
        camera_distance_ = clear_camera_distance();
    }

    [[nodiscard]] Vec3 orbit_direction() const {
        const float horizontal = std::cos(orbit_.pitch_radians);
        return {horizontal * std::cos(orbit_.yaw_radians), horizontal * std::sin(orbit_.yaw_radians),
                std::sin(orbit_.pitch_radians)};
    }

    [[nodiscard]] Vec3 orbit_target() const {
        const auto feet = course_position();
        return {feet.x, feet.y, feet.z + motor_->config().height * 0.58f};
    }

    [[nodiscard]] float clear_camera_distance() const {
        const auto target = orbit_target();
        const auto direction = orbit_direction();
        constexpr float radius = 0.12f;
        const Capsule sphere{{target.x, target.y, target.z - radius}, radius, 2.0f * radius};
        const Vec3 movement{-direction.x * orbit_distance, -direction.y * orbit_distance, -direction.z * orbit_distance};
        const auto box = sweep_capsule_boxes(sphere, movement, collision_.boxes());
        const auto triangle = sweep_capsule_triangles(sphere, movement, collision_.triangles());
        const float box_t = box ? box->t : 1.0f;
        const float triangle_t = triangle ? triangle->t : 1.0f;
        return box || triangle ? std::max(0.0f, std::min(box_t, triangle_t) * orbit_distance - 0.03f) : orbit_distance;
    }

    void update_camera_distance(std::chrono::nanoseconds step) {
        constexpr float extension_speed = 12.0f;
        const float permitted = clear_camera_distance();
        const float extension = extension_speed * std::chrono::duration<float>(step).count();
        camera_distance_ = std::min(permitted, camera_distance_ + extension);
    }

    void reset_course(bool jump_held) {
        motor_->reset();
        driving_ = false;
        if (vehicle_) {
            if (vehicle_spawn_) vehicle_->reset();
            else vehicle_->park();
        }
        orbit_ = initial_orbit_;
        suppress_jump_until_release_ = jump_held;
        camera_distance_ = clear_camera_distance();
    }

    static constexpr float orbit_distance = 6.0f;
    static constexpr FreeCamera initial_orbit_{.pitch_radians = -0.35f};
    Entity marker_;
    IDesktopPlatform& platform_;
    Vec3 position_{};
    std::optional<CapsuleCharacter> motor_;
    StaticCollision collision_;
    QueryScratch vehicle_scratch_;
    bool terrain_vehicle_{};
    FreeCamera orbit_{initial_orbit_};
    float camera_distance_{orbit_distance};
    bool suppress_jump_until_release_{};
    std::optional<CourseVehicle> vehicle_;
    std::optional<Vec3> vehicle_spawn_;
    Entity vehicle_marker_{};
    bool driving_{};
    bool interact_held_{};
};

} // namespace saige::sample
