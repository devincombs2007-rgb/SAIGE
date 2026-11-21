#include "saige/gfx_assets/gltf.hpp"
#include "saige/project_session.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: saige_scene_asset_tool PROJECT\n";
        return 2;
    }

    try {
        saige::ProjectSession session{std::filesystem::path(argv[1])};
        std::size_t primitives = 0;
        std::size_t images = 0;
        std::size_t image_bytes = 0;
        for (const auto& entry : session.source().assets) {
            const auto relative = std::filesystem::path(std::u8string(entry.relative_path.begin(),
                                                                     entry.relative_path.end()));
            const auto loaded = saige::gfx::load_gltf_file(session.root() / relative);
            if (!loaded.ok()) {
                throw std::runtime_error(entry.relative_path + ": " + loaded.error);
            }
            for (const auto& mesh : loaded.asset->meshes) primitives += mesh.primitives.size();
            images += loaded.asset->images.size();
            for (const auto& image : loaded.asset->images) image_bytes += image.bytes.size();
        }
        std::cout << "{\"schema_version\":1,\"scene_nodes\":" << session.source().nodes.size()
                  << ",\"unique_assets\":" << session.source().assets.size()
                  << ",\"mesh_primitives\":" << primitives << ",\"encoded_images\":" << images
                  << ",\"encoded_image_bytes\":" << image_bytes << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
