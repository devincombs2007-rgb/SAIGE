#include "saige/chunk_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace saige {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t max_chunk_file_bytes = 1024 * 1024;
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

fs::path read_relative_chunk_path(const Json& field, const fs::path& root) {
    if (!field.is_string()) throw std::invalid_argument("Chunk file must be a relative path");
    const auto& name = field.get_ref<const std::string&>();
    if (name.empty() || name.find_first_of("\\:<>|\"?*") != std::string::npos ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32; })) {
        throw std::invalid_argument("Chunk file must be a portable relative path");
    }
    std::string_view rest(name);
    for (;;) {
        const auto slash = rest.find('/');
        const auto component = rest.substr(0, slash);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ') {
            throw std::invalid_argument("Chunk file path contains a disallowed component");
        }
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }

    const auto path = fs::weakly_canonical(root / fs::path(std::u8string(name.begin(), name.end())));
    const auto [root_end, relative_begin] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    if (root_end != root.end() || relative_begin == path.end()) {
        throw std::invalid_argument("Chunk file escapes the manifest directory");
    }
    return path;
}

Json read_chunk_document(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open chunk file");
    std::string bytes(max_chunk_file_bytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || static_cast<std::size_t>(input.gcount()) > max_chunk_file_bytes) {
        throw std::runtime_error("Chunk file exceeds its size limit or could not be read");
    }
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 8) throw std::invalid_argument("Chunk file nesting limit exceeded");
        return true;
    }, false);
}

Vec3 read_box_corner(const Json& corner) {
    if (!corner.is_array() || corner.size() != 3) throw std::invalid_argument("Expected an XYZ box corner");
    const auto component = [](const Json& value) {
        constexpr auto limit = std::numeric_limits<float>::max();
        if (!value.is_number()) throw std::invalid_argument("Expected a numeric box coordinate");
        const auto coordinate = value.get<double>();
        if (!std::isfinite(coordinate) || coordinate < -limit || coordinate > limit) {
            throw std::invalid_argument("Box coordinate is outside the float range");
        }
        return static_cast<float>(coordinate);
    };
    return {component(corner[0]), component(corner[1]), component(corner[2])};
}

} // namespace

std::vector<ChunkSource> load_chunk_manifest(const fs::path& path) {
    const auto document = read_chunk_document(path);
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
        if ((!has_fields(entry, {"coord", "project"}) && !has_fields(entry, {"coord", "project", "collision"})) ||
            !entry["coord"].is_array() || entry["coord"].size() != 2) {
            throw std::invalid_argument("Expected a chunk coordinate pair and project path");
        }
        const auto& coord = entry["coord"];
        const auto collision = entry.contains("collision") ? read_relative_chunk_path(entry["collision"], root) : fs::path{};
        result.push_back({{read_coord(coord[0]), read_coord(coord[1])}, read_relative_chunk_path(entry["project"], root), collision});
    }
    std::ranges::sort(result, {}, &ChunkSource::coord);
    if (std::ranges::adjacent_find(result, {}, &ChunkSource::coord) != result.end()) {
        throw std::invalid_argument("Duplicate chunk coordinates");
    }
    return result;
}

std::vector<Aabb> load_chunk_collision(const fs::path& path) {
    constexpr std::size_t max_collision_boxes = 4096;
    const auto document = read_chunk_document(path);
    if (!has_fields(document, {"schema_version", "boxes"}) ||
        !document["schema_version"].is_number_integer() || document["schema_version"] != 1) {
        throw std::invalid_argument("Expected a version 1 chunk collision file");
    }
    const auto& boxes = document["boxes"];
    if (!boxes.is_array() || boxes.size() > max_collision_boxes) {
        throw std::invalid_argument("Expected a bounded collision box array");
    }
    std::vector<Aabb> result;
    result.reserve(boxes.size());
    for (const auto& entry : boxes) {
        if (!has_fields(entry, {"min", "max"})) throw std::invalid_argument("Expected min and max box corners");
        const Aabb box{read_box_corner(entry["min"]), read_box_corner(entry["max"])};
        if (box.min.x >= box.max.x || box.min.y >= box.max.y || box.min.z >= box.max.z) {
            throw std::invalid_argument("Collision box must have positive extent on every axis");
        }
        result.push_back(box);
    }
    return result;
}

ChunkCoord chunk_containing(Vec3 position, double cell_size) {
    if (!std::isfinite(cell_size) || cell_size <= 0.0 || !std::isfinite(position.x) ||
        !std::isfinite(position.y) || !std::isfinite(position.z)) {
        throw std::invalid_argument("Chunk lookup requires a finite position and positive cell size");
    }
    const auto cell = [cell_size](float coordinate) {
        const double index = std::floor(static_cast<double>(coordinate) / cell_size);
        if (index < std::numeric_limits<std::int32_t>::min() || index > std::numeric_limits<std::int32_t>::max()) {
            throw std::out_of_range("World position is outside the chunk grid");
        }
        return static_cast<std::int32_t>(index);
    };
    return {cell(position.x), cell(position.y)};
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
