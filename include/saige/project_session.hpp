#pragma once

#include "saige/desktop.hpp"
#include "saige/free_camera.hpp"
#include "saige/project.hpp"

namespace saige {

// Owns a validated CPU scene and its runtime world. Reload between calls to run.
// Entity handles and asset paths from an earlier scene belong to that scene.
class ProjectSession final : public IGame {
public:
    explicit ProjectSession(const std::filesystem::path& path, IDesktopPlatform* desktop = nullptr);

    void reload(const std::filesystem::path& path);
    // The caller owns the platform. Attaching a platform discards its buffered drag motion.
    void attach_desktop(IDesktopPlatform* desktop) noexcept;
    [[nodiscard]] World& world() noexcept { return world_; }
    [[nodiscard]] const Project& source() const noexcept { return source_; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    void fixed_update(World&, std::chrono::nanoseconds step) override;
    [[nodiscard]] std::optional<CameraView> camera_view() const override;

private:
    Project source_;
    std::filesystem::path root_;
    World world_;
    FreeCamera camera_;
    Quaternion initial_orientation_;
    bool has_looked_{};
    IDesktopPlatform* desktop_;
};

} // namespace saige
