#include "saige/project.hpp"
#include "atomic_save.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace saige {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using IdKey = std::pair<std::uint64_t, std::uint64_t>;
constexpr std::size_t max_file_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_entries = 16'384;

IdKey key(AssetId id) { return {id.high, id.low}; }

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

void require_fields(const Json& entry, std::initializer_list<std::string_view> fields) {
    if (!entry.is_object() || entry.size() != fields.size()) {
        invalid("Project object has missing or unexpected fields");
    }
    for (const auto field : fields) {
        if (!entry.contains(field)) invalid("Missing project field: " + std::string(field));
    }
}

AssetId read_id(const Json& value) {
    if (!value.is_string()) invalid("Project ID must be a canonical string");
    auto id = parse_asset_id(value.get_ref<const std::string&>());
    if (!id) invalid("Invalid project ID");
    return *id;
}

float read_number(const Json& value) {
    if (!value.is_number()) invalid("Expected a finite number");
    const auto number = value.get<float>();
    if (!std::isfinite(number)) invalid("Project number is out of range");
    return number;
}

template<std::size_t Count>
std::array<float, Count> read_vector(const Json& value) {
    if (!value.is_array() || value.size() != Count) invalid("Project vector has the wrong size");
    std::array<float, Count> components{};
    for (std::size_t i = 0; i < Count; ++i) components[i] = read_number(value[i]);
    return components;
}

Vec3 read_vec3(const Json& value) {
    const auto [x, y, z] = read_vector<3>(value);
    return {x, y, z};
}

Quaternion read_quat(const Json& value) {
    const auto [x, y, z, w] = read_vector<4>(value);
    return {x, y, z, w};
}

Json json_vec(Vec3 value) { return Json::array({value.x, value.y, value.z}); }
Json json_quat(Quaternion value) { return Json::array({value.x, value.y, value.z, value.w}); }

void validate_path(std::string_view relative, const fs::path& root) {
    if (relative.empty() || relative.front() == '/' ||
        relative.find_first_of("\\:<>|\"?*") != std::string_view::npos ||
        std::ranges::any_of(relative, [](unsigned char c) { return c < 32; })) {
        invalid("Asset path must be project-relative");
    }
    auto remaining = relative;
    for (;;) {
        const auto slash = remaining.find('/');
        const auto part = remaining.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') {
            invalid("Asset path contains a disallowed component");
        }
        if (slash == std::string_view::npos) break;
        remaining.remove_prefix(slash + 1);
    }
    const auto resolved = fs::canonical(root / fs::path(std::u8string(relative.begin(), relative.end())));
    const auto [root_end, path_cursor] = std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end());
    if (root_end != root.end() || path_cursor == resolved.end() || !fs::is_regular_file(resolved)) {
        invalid("Asset escapes the project root or is not a regular file");
    }
}

void validate(const Project& project, const fs::path& root) {
    if (project.assets.size() > max_entries || project.nodes.size() > max_entries) {
        invalid("Project entry limit exceeded");
    }
    std::set<IdKey> assets;
    for (const auto& asset : project.assets) {
        if (asset.id.is_builtin() || !assets.insert(key(asset.id)).second) invalid("Duplicate or reserved asset ID");
        validate_path(asset.relative_path, root);
    }
    for (const auto& node : project.nodes) {
        if (!node.mesh.is_builtin() && !assets.contains(key(node.mesh))) invalid("Missing project mesh ID");
    }
    (void)resolve_project_matrices(project);

    const auto& camera = project.camera;
    if (!(camera.fov_y_radians > 0 && camera.fov_y_radians < 3.14159265F &&
          camera.near_plane > 0 && std::isfinite(camera.near_plane))) {
        invalid("Invalid project camera projection");
    }
    const SceneNode pose{{camera.position, camera.rotation}};
    (void)resolve_world_matrices(std::span(&pose, 1));
}

Project decode(const Json& document) {
    require_fields(document, {"schema_version", "assets", "nodes", "camera"});
    if (!document.at("schema_version").is_number_integer() || document.at("schema_version") != 1) {
        invalid("Unsupported project schema version");
    }
    const auto& assets = document.at("assets");
    const auto& nodes = document.at("nodes");
    if (!assets.is_array() || !nodes.is_array() || assets.size() > max_entries || nodes.size() > max_entries) {
        invalid("Invalid or oversized project tables");
    }
    Project project;
    project.assets.reserve(assets.size());
    project.nodes.reserve(nodes.size());
    for (const auto& entry : assets) {
        require_fields(entry, {"id", "path"});
        project.assets.push_back({read_id(entry.at("id")), entry.at("path").get<std::string>()});
    }
    for (const auto& entry : nodes) {
        require_fields(entry, {"id", "parent", "mesh", "position", "rotation", "scale"});
        std::optional<AssetId> parent;
        if (!entry.at("parent").is_null()) parent = read_id(entry.at("parent"));
        project.nodes.push_back({read_id(entry.at("id")), parent, read_id(entry.at("mesh")),
                                 {read_vec3(entry.at("position")), read_quat(entry.at("rotation")),
                                  read_vec3(entry.at("scale"))}});
    }
    const auto& camera = document.at("camera");
    require_fields(camera, {"position", "rotation", "fov_y_radians", "near_plane"});
    project.camera = {read_vec3(camera.at("position")), read_quat(camera.at("rotation")),
                      read_number(camera.at("fov_y_radians")), read_number(camera.at("near_plane"))};
    return project;
}

Json encode(const Project& project) {
    auto assets = Json::array();
    for (const auto& asset : project.assets) {
        assets.push_back({{"id", format_asset_id(asset.id)}, {"path", asset.relative_path}});
    }
    auto nodes = Json::array();
    for (const auto& node : project.nodes) {
        nodes.push_back({{"id", format_asset_id(node.id)},
                         {"parent", node.parent ? Json(format_asset_id(*node.parent)) : Json(nullptr)},
                         {"mesh", format_asset_id(node.mesh)}, {"position", json_vec(node.local.position)},
                         {"rotation", json_quat(node.local.rotation)}, {"scale", json_vec(node.local.scale)}});
    }
    const auto& camera = project.camera;
    return {{"schema_version", 1}, {"assets", std::move(assets)}, {"nodes", std::move(nodes)},
            {"camera", {{"position", json_vec(camera.position)}, {"rotation", json_quat(camera.rotation)},
                        {"fov_y_radians", camera.fov_y_radians}, {"near_plane", camera.near_plane}}}};
}

} // namespace

std::vector<WorldMatrix> resolve_project_matrices(const Project& project) {
    std::map<IdKey, std::size_t> indices;
    for (std::size_t i = 0; i < project.nodes.size(); ++i) {
        const auto id = project.nodes[i].id;
        if (id.is_builtin() || !indices.emplace(key(id), i).second) invalid("Duplicate or reserved node ID");
    }
    std::vector<SceneNode> nodes;
    nodes.reserve(project.nodes.size());
    for (const auto& entry : project.nodes) {
        std::optional<std::size_t> parent;
        if (entry.parent) {
            const auto found = indices.find(key(*entry.parent));
            if (found == indices.end()) invalid("Missing project parent ID");
            parent = found->second;
        }
        nodes.push_back({entry.local, parent});
    }
    return resolve_world_matrices(nodes);
}

Project load_project(const fs::path& path) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open project");
        std::string bytes;
        std::array<char, 16 * 1024> chunk{};
        while (stream) {
            stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = static_cast<std::size_t>(stream.gcount());
            if (count > max_file_bytes - bytes.size()) invalid("Project exceeds the read limit");
            bytes.append(chunk.data(), count);
        }
        if (!stream.eof() || stream.bad()) invalid("Project failed to read");

        std::vector<std::set<std::string>> object_keys;
        const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 16) invalid("Project JSON nesting exceeds limit");
            if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
            if (event == Json::parse_event_t::object_end) object_keys.pop_back();
            if (event == Json::parse_event_t::key &&
                !object_keys.back().insert(value.get_ref<const std::string&>()).second) {
                invalid("Duplicate project JSON key");
            }
            return true;
        };
        auto project = decode(Json::parse(bytes, callback));
        validate(project, fs::canonical(fs::absolute(path).parent_path()));
        return project;
    } catch (const std::exception& error) {
        throw std::runtime_error("Cannot load project: " + std::string(error.what()));
    }
}

void save_project(const Project& project, const fs::path& path) {
    const auto root = fs::canonical(fs::absolute(path).parent_path());
    validate(project, root);
    const auto wire = encode(project).dump(2) + '\n';
    if (wire.size() > max_file_bytes) invalid("Serialized project exceeds file limit");

    detail::replace_save_file(root / path.filename(), wire);
}

} // namespace saige
