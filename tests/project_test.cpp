#include "saige/project.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Operation operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid project was accepted");
}

struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / ("saige-project-test-" + std::to_string(std::random_device{}()));
    TemporaryDirectory() { require(fs::create_directory(path), "Cannot create test directory"); }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    require(static_cast<bool>(file), "Could not write project fixture");
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void run() {
    TemporaryDirectory temporary;
    const auto root = temporary.path / "project";
    require(fs::create_directory(root), "Cannot create project folder");
    const auto file = root / "scene.json";
    const auto bad = root / "bad.json";
    write(root / "mesh.gltf", R"({"asset":{"version":"2.0"}})");
    write(temporary.path / "outside.gltf", "{}");

    saige::Project project;
    const saige::AssetId mesh{0, 5}, parent{1, 2}, child{1, 3};
    project.assets = {{mesh, "mesh.gltf"}};
    project.nodes = {{child, parent, mesh, {{1, 2, 3}}}, {parent, {}, {}, {{3, 0, 0}}}};
    project.camera.position = {-5, 1, 2};
    saige::save_project(project, file);
    auto active = saige::load_project(file);
    const auto original = read(file);
    require(active.nodes.size() == 2 && active.nodes.front().parent == parent &&
                active.nodes.front().mesh == mesh && active.assets.front().relative_path == "mesh.gltf" &&
                active.camera.position == project.camera.position,
            "Project data did not round-trip");
    require(saige::resolve_project_matrices(active)[0][12] == 4.0F, "Loaded hierarchy was not resolved");
    saige::save_project(active, file);
    require(read(file) == original, "Repeat save changed serialized form");

    write(bad, "{\"schema_version\":1,\"schema_version\":2}");
    rejects([&] { active = saige::load_project(bad); });
    require(active.nodes.front().id == child, "Failed load replaced the active project");
    write(bad, "[");
    rejects([&] { (void)saige::load_project(bad); });

    project.nodes[1].parent = child;
    rejects([&] { saige::save_project(project, file); });
    project.nodes[1].parent.reset();
    project.nodes[0].mesh = {5, 1};
    rejects([&] { saige::save_project(project, file); });
    project.nodes[0].mesh = mesh;
    project.camera.near_plane = 0;
    rejects([&] { saige::save_project(project, file); });
    project.camera.near_plane = 0.1F;

    for (const char* unsafe : {"../outside.gltf", "C:\\mesh.gltf", "missing.gltf", "mesh.gltf."}) {
        project.assets[0].relative_path = unsafe;
        rejects([&] { saige::save_project(project, file); });
    }
    std::error_code link_error;
    fs::create_symlink(temporary.path / "outside.gltf", root / "escape.gltf", link_error);
    if (!link_error) {
        project.assets[0].relative_path = "escape.gltf";
        rejects([&] { saige::save_project(project, file); });
    }
    require(read(file) == original, "An invalid save clobbered the existing project");
    fs::remove(root / "mesh.gltf");
    rejects([&] { (void)saige::load_project(file); });
    write(root / "mesh.gltf", "{}");
    require(saige::load_project(file).nodes.size() == 2, "Restoring a resource did not recover the project");
}
} // namespace

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
