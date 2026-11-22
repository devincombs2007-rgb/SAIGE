#include "saige/chunk_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace saige {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t max_manifest_bytes = 1024 * 1024;
constexpr std::size_t max_manifest_chunks = 4096;

bool has_fields(const Json& object, std::initializer_list<std::string_view> names) {
    return object.is_object() && object.size() == names.size() &&
        std::ranges::all_of(names, [&](std::string_view name) { return object.contains(name); });
}

std::int32_t read_coord(const Json& coordinate) {
    constexpr auto min = std::numeric_limits<std::int32_t>::min();
    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    if (coordinate.is_number_unsigned()) {
        const auto value = coordinate.get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(max)) return static_cast<std::int32_t>(value);
    } else if (coordinate.is_number_integer()) {
        const auto value = coordinate.get<std::int64_t>();
        if (value >= min && value <= max) return static_cast<std::int32_t>(value);
    }
    throw std::invalid_argument("Chunk coordinates must be signed 32-bit integers");
}

fs::path read_relative_project(const Json& field, const fs::path& root) {
    if (!field.is_string()) throw std::invalid_argument("Chunk project must be a relative path");
    const auto& name = field.get_ref<const std::string&>();
    if (name.empty() || name.find_first_of("\\:<>|\"?*") != std::string::npos ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32; })) {
        throw std::invalid_argument("Chunk project must be a portable relative path");
    }
    std::string_view rest(name);
    for (;;) {
        const auto slash = rest.find('/');
        const auto component = rest.substr(0, slash);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ') {
            throw std::invalid_argument("Chunk project path contains a disallowed component");
        }
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }

    const auto path = fs::weakly_canonical(root / fs::path(std::u8string(name.begin(), name.end())));
    const auto [root_end, relative_begin] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    if (root_end != root.end() || relative_begin == path.end()) {
        throw std::invalid_argument("Chunk project escapes the manifest directory");
    }
    return path;
}

} // namespace

std::vector<ChunkSource> load_chunk_manifest(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open chunk manifest");
    std::string bytes(max_manifest_bytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || static_cast<std::size_t>(input.gcount()) > max_manifest_bytes) {
        throw std::runtime_error("Chunk manifest exceeds its size limit or could not be read");
    }
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    const auto document = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 8) throw std::invalid_argument("Chunk manifest nesting limit exceeded");
        return true;
    }, false);
    if (!has_fields(document, {"schema_version", "chunks"}) ||
        !document["schema_version"].is_number_integer() || document["schema_version"] != 1) {
        throw std::invalid_argument("Expected a version 1 chunk manifest");
    }
    const auto& entries = document["chunks"];
    if (!entries.is_array() || entries.size() > max_manifest_chunks) {
        throw std::invalid_argument("Expected a bounded chunk array");
    }
    const auto root = fs::canonical(fs::absolute(path).parent_path());
    std::vector<ChunkSource> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!has_fields(entry, {"coord", "project"}) || !entry["coord"].is_array() || entry["coord"].size() != 2) {
            throw std::invalid_argument("Expected a chunk coordinate pair and project path");
        }
        const auto& coord = entry["coord"];
        result.push_back({{read_coord(coord[0]), read_coord(coord[1])}, read_relative_project(entry["project"], root)});
    }
    std::ranges::sort(result, {}, &ChunkSource::coord);
    if (std::ranges::adjacent_find(result, {}, &ChunkSource::coord) != result.end()) {
        throw std::invalid_argument("Duplicate chunk coordinates");
    }
    return result;
}

std::vector<ChunkSource> select_chunks(std::span<const ChunkSource> sources, ChunkCoord center, std::uint32_t radius) {
    std::vector<ChunkSource> selected;
    for (const auto& source : sources) {
        const auto x = static_cast<std::int64_t>(source.coord.x) - center.x;
        const auto y = static_cast<std::int64_t>(source.coord.y) - center.y;
        if (std::abs(x) <= radius && std::abs(y) <= radius) {
            selected.push_back(source);
        }
    }
    std::ranges::sort(selected, {}, &ChunkSource::coord);
    return selected;
}

} // namespace saige
