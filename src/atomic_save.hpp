#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace saige::detail {

inline void replace_save_file(const std::filesystem::path& path, std::string_view wire) {
    namespace fs = std::filesystem;
    const auto root = fs::canonical(fs::absolute(path).parent_path());
    struct SaveStaging {
        fs::path directory;
        ~SaveStaging() {
            if (!directory.empty()) {
                std::error_code ignored;
                fs::remove_all(directory, ignored);
            }
        }
    };
    // Stage on the same filesystem; replace the destination only after close succeeds.
    SaveStaging staging;
    std::random_device random;
    for (int attempts = 0; attempts < 16 && staging.directory.empty(); ++attempts) {
        auto directory = root / (".saige-save-" + std::to_string(random()));
        if (fs::create_directory(directory)) staging.directory = std::move(directory);
    }
    if (staging.directory.empty()) throw std::runtime_error("Cannot create save staging directory");
    const auto temporary = staging.directory / "payload";
    std::ofstream output(temporary, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(wire.data(), static_cast<std::streamsize>(wire.size()));
    output.close();
    fs::rename(temporary, root / path.filename());
}

} // namespace saige::detail
