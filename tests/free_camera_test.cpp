#include "saige/free_camera.hpp"

#include <cmath>
#include <exception>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

bool near(float left, float right) {
    return std::abs(left - right) < 0.0001F;
}

void test_movement() {
    saige::FreeCamera camera;
    camera.update({.forward = 1, .right = 1}, 1s);
    require(near(camera.position.x, -camera.position.y) &&
                near(std::hypot(camera.position.x, camera.position.y), camera.speed_mps),
            "Diagonal movement differs from the configured travel speed");
    camera.position = {};
    camera.radians_per_pixel = std::numbers::pi_v<float> / 200.F;
    camera.update({.forward = 1, .look_x_pixels = -100}, 1s);
    require(near(camera.position.x, 0.F) && near(camera.position.y, 4.F),
            "Movement did not follow the turned camera");
    const auto rotation = camera.view().rotation;
    require(near(rotation.z, std::sqrt(0.5F)) && near(rotation.w, rotation.z),
            "Camera view did not expose the expected yaw quaternion");
    camera.update({.look_y_pixels = -10000}, 1s);
    require(camera.pitch_radians < std::numbers::pi_v<float> / 2.F,
            "Pitch crossed its vertical limit");
}

void test_render_pose() {
    saige::World world;
    auto entity = world.create();
    const saige::WorldMatrix affine{1, 0, 0, 0, 0.5F, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1};
    require(world.set_render_pose(entity, affine, {1, 2}), "World rejected an affine pose");
    require(world.set_position(entity, {7, 8, 9}), "Translation update failed");
    const auto pose = world.snapshot().front();
    require(pose.mesh == saige::AssetId{1, 2} && pose.world_matrix &&
                (*pose.world_matrix)[4] == 0.5F && (*pose.world_matrix)[12] == 7.F &&
                pose.position == saige::Vec3{7, 8, 9},
            "Snapshot lost shear, translation or mesh identity");
    require(world.destroy(entity), "Could not remove entity");
    require(!world.set_render_pose(entity, affine, {}), "Expired entity accepted a render pose");
    (void)world.create();
    const auto fresh = world.snapshot().front();
    require(!fresh.world_matrix && fresh.mesh.is_builtin(), "Recycled slot retained render resources");
}
} // namespace

int main() {
    try {
        test_movement();
        test_render_pose();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
