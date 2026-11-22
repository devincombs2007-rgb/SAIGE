#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/project_session.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace saige {

struct PreparedAsset {
    AssetId id;
    gfx::GltfAsset model;
};

// Candidate for native scene startup or replacement. Construction checks the
// project and imports every CPU asset. It does not touch input or GPU state.
class PreparedScene {
public:
    explicit PreparedScene(const std::filesystem::path& path,
                           std::size_t max_asset_payload_bytes = std::size_t{256} << 20);

    [[nodiscard]] const Project& project() const noexcept { return session_.source(); }
    [[nodiscard]] std::span<const PreparedAsset> assets() const noexcept { return assets_; }
    [[nodiscard]] const gfx::GltfAsset* find_asset(AssetId id) const noexcept;
    [[nodiscard]] std::size_t asset_payload_bytes() const noexcept { return asset_payload_bytes_; }

    // Input is borrowed only while run is active. Bind the sink after successful
    // GPU preparation and destroy its resources before destroying its platform.
    RunStats run(IDesktopPlatform& desktop, IFrameSink& sink,
                 std::optional<std::uint64_t> frame_budget = std::nullopt,
                 ICpuFrameObserver* observer = nullptr);

private:
    ProjectSession session_;
    std::vector<PreparedAsset> assets_;
    std::size_t asset_payload_bytes_{};
};

} // namespace saige
