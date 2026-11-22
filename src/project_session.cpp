#include "saige/project_session.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace saige {
namespace {
Quaternion normalized(Quaternion q) {
    const double length = std::hypot(std::hypot(static_cast<double>(q.x), static_cast<double>(q.y)),
                                     std::hypot(static_cast<double>(q.z), static_cast<double>(q.w)));
    return {static_cast<float>(q.x / length), static_cast<float>(q.y / length),
            static_cast<float>(q.z / length), static_cast<float>(q.w / length)};
}

FreeCamera from_project_pose(const ProjectCamera& pose, Quaternion q) {
    const float forward_x = 1.F - 2.F * (q.y * q.y + q.z * q.z);
    const float forward_y = 2.F * (q.x * q.y + q.z * q.w);
    const float forward_z = 2.F * (q.x * q.z - q.y * q.w);
    return {.position = pose.position,
            .yaw_radians = std::atan2(forward_y, forward_x),
            .pitch_radians = std::asin(std::clamp(forward_z, -1.F, 1.F)),
            .fov_y_radians = pose.fov_y_radians,
            .near_plane = pose.near_plane};
}
} // namespace

ProjectSession::ProjectSession(const std::filesystem::path& path, IDesktopPlatform* desktop)
    : desktop_(desktop) {
    reload(path);
}

void ProjectSession::reload(const std::filesystem::path& path) {
    auto root = std::filesystem::canonical(std::filesystem::absolute(path).parent_path());
    auto source = load_project(path);
    const auto matrices = resolve_project_matrices(source);
    World world;
    for (std::size_t i = 0; i < matrices.size(); ++i) {
        const auto entity = world.create();
        if (!world.set_render_pose(entity, matrices[i], source.nodes[i].mesh)) {
            throw std::runtime_error("Validated project pose could not be extracted");
        }
    }
    const auto orientation = normalized(source.camera.rotation);
    const auto camera = from_project_pose(source.camera, orientation);

    static_assert(std::is_nothrow_move_assignable_v<Project> && std::is_nothrow_move_assignable_v<World>);
    root_ = std::move(root);
    source_ = std::move(source);
    world_ = std::move(world);
    camera_ = camera;
    initial_orientation_ = orientation;
    has_looked_ = false;
    if (desktop_) (void)desktop_->consume_look_delta();
}

void ProjectSession::attach_desktop(IDesktopPlatform* desktop) noexcept {
    if (desktop_ != desktop) {
        desktop_ = desktop;
        if (desktop_) (void)desktop_->consume_look_delta();
    }
}

void ProjectSession::fixed_update(World&, std::chrono::nanoseconds step) {
    if (!desktop_ || step <= std::chrono::nanoseconds::zero()) return;
    const auto down = [&](Key key) { return desktop_->key_down(key) ? 1.F : 0.F; };
    const auto look = desktop_->consume_look_delta();
    const CameraInput input{down(Key::w) - down(Key::s), down(Key::d) - down(Key::a),
                            down(Key::up) - down(Key::down), look.x_pixels, look.y_pixels};
    if (look.x_pixels != 0 || look.y_pixels != 0) has_looked_ = true;
    if (input.forward != 0 || input.right != 0 || input.rise != 0 || look.x_pixels != 0 || look.y_pixels != 0) {
        camera_.update(input, step);
    }
}

std::optional<CameraView> ProjectSession::camera_view() const {
    auto view = camera_.view();
    if (!has_looked_) view.rotation = initial_orientation_;
    return view;
}

} // namespace saige
