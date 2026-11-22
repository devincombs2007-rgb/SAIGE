#include "saige/prepared_scene.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: saige_scene_asset_tool PROJECT\n";
        return 2;
    }

    try {
        saige::PreparedScene scene{std::filesystem::path(argv[1])};
        std::size_t primitives = 0;
        std::size_t images = 0;
        std::size_t image_bytes = 0;
        for (const auto& asset : scene.assets()) {
            for (const auto& mesh : asset.model.meshes) primitives += mesh.primitives.size();
            images += asset.model.images.size();
            for (const auto& image : asset.model.images) image_bytes += image.bytes.size();
        }
        std::cout << "{\"schema_version\":1,\"scene_nodes\":" << scene.project().nodes.size()
                  << ",\"unique_assets\":" << scene.assets().size()
                  << ",\"mesh_primitives\":" << primitives << ",\"encoded_images\":" << images
                  << ",\"encoded_image_bytes\":" << image_bytes
                  << ",\"asset_payload_bytes\":" << scene.asset_payload_bytes() << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
