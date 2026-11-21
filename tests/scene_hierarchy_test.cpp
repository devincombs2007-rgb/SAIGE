#include "saige/scene_hierarchy.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(float value, float expected) {
    return std::abs(value - expected) < 0.0001F;
}

void require_rejection(std::span<const saige::SceneNode> nodes) {
    try {
        (void)saige::resolve_world_matrices(nodes);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid hierarchy was accepted");
}

void test_composition() {
    std::array<saige::SceneNode, 3> nodes{};
    nodes[0] = {{{1, 0, 0}, {0, 0, 1, 2}, {1, 1, 1}}, 1};
    nodes[1].local.position = {4, 5, 6};
    nodes[1].local.scale = {2, 1, 1};
    nodes[2].local.position = {0, 0, 3};
    const auto matrices = saige::resolve_world_matrices(nodes);
    const auto& nested = matrices[0];
    require(near(nested[12], 6) && near(nested[13], 5) && near(nested[14], 6),
            "The child translation did not inherit parent scale");
    require(near(nested[0], 1.2F) && near(nested[1], 0.8F) &&
                near(nested[4], -1.6F) && near(nested[5], 0.6F),
            "The composed matrix lost normalized rotation or inherited shear");
    require(near(matrices[2][14], 3), "Independent root moved with the hierarchy");
    nodes[1].local.position.x = 8;
    require(near(saige::resolve_world_matrices(nodes)[0][12], 10),
            "The parent edit did not reach the child");
}

void test_validation_and_depth() {
    std::array<saige::SceneNode, 2> nodes{};
    nodes[0].parent = 3;
    require_rejection(nodes);
    nodes[0].parent = 1;
    nodes[1].parent = 0;
    require_rejection(nodes);
    nodes[1].parent.reset();
    nodes[1].local.scale.x = -1;
    require_rejection(nodes);
    nodes[1].local.scale.x = 1;
    nodes[1].local.rotation.z = std::numeric_limits<float>::quiet_NaN();
    require_rejection(nodes);

    std::vector<saige::SceneNode> chain(4096);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        chain[i].local.position.x = 1;
        if (i + 1 < chain.size()) {
            chain[i].parent = i + 1;
        }
    }
    const auto matrices = saige::resolve_world_matrices(chain);
    require(near(matrices.front()[12], 4096), "Deep hierarchy did not resolve");
}

} // namespace

int main() {
    try {
        test_composition();
        test_validation_and_depth();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
