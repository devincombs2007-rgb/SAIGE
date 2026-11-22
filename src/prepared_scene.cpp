#include "saige/prepared_scene.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace saige {
namespace {

auto order(AssetId id) noexcept { return std::pair{id.high, id.low}; }

void account_model_payload(const gfx::GltfAsset& asset, std::size_t& bytes, std::size_t limit) {
    const auto account = [&bytes, limit](const auto& payload) {
        constexpr auto element_bytes = sizeof(typename std::decay_t<decltype(payload)>::value_type);
        if (payload.size() > (limit - bytes) / element_bytes) {
            throw std::runtime_error("Prepared scene exceeds the imported asset payload limit");
        }
        bytes += payload.size() * element_bytes;
    };
    for (const auto& mesh : asset.meshes) {
        for (const auto& primitive : mesh.primitives) {
            account(primitive.positions);
            account(primitive.normals);
            account(primitive.tangents);
            account(primitive.uv0);
            account(primitive.colors);
            account(primitive.indices);
        }
    }
    for (const auto& image : asset.images) account(image.bytes);
}

} // namespace

PreparedScene::PreparedScene(const std::filesystem::path& path, std::size_t max_asset_payload_bytes)
    : session_(path) {
    const auto& sources = session_.source().assets;
    assets_.reserve(sources.size());
    for (const auto& entry : sources) {
        const auto relative = std::filesystem::path(
            std::u8string(entry.relative_path.begin(), entry.relative_path.end()));
        auto loaded = gfx::load_gltf_file(session_.root() / relative);
        if (!loaded.ok()) throw std::runtime_error(entry.relative_path + ": " + loaded.error);
        account_model_payload(*loaded.asset, asset_payload_bytes_, max_asset_payload_bytes);
        assets_.push_back({entry.id, std::move(*loaded.asset)});
    }
    std::ranges::sort(assets_, {}, [](const PreparedAsset& asset) { return order(asset.id); });
}

const gfx::GltfAsset* PreparedScene::find_asset(AssetId id) const noexcept {
    const auto it = std::ranges::lower_bound(assets_, order(id), {},
        [](const PreparedAsset& asset) { return order(asset.id); });
    return it != assets_.end() && it->id == id ? &it->model : nullptr;
}

RunStats PreparedScene::run(IDesktopPlatform& desktop, IFrameSink& sink,
                            std::optional<std::uint64_t> frame_budget) {
    if (frame_budget == 0) return {};

    struct InputBinding {
        ProjectSession& session;
        ~InputBinding() { session.attach_desktop(nullptr); }
    } binding{session_};
    session_.attach_desktop(&desktop);
    return saige::run(session_.world(), desktop, session_, sink, frame_budget);
}

} // namespace saige
