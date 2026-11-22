#pragma once

#include "saige/asset_id.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace saige {

// A handle belongs to the World that issued it. Generation zero is invalid.
struct Entity {
    std::uint32_t index{};
    std::uint32_t generation{};
    bool operator==(const Entity&) const = default;
};

struct Vec3 {
    float x{}, y{}, z{};
    bool operator==(const Vec3&) const = default;
};

// Column-major affine matrix. Translation is stored at indices 12–14.
using WorldMatrix = std::array<float, 16>;

struct RenderInstance {
    Entity entity;
    Vec3 position;
    std::optional<WorldMatrix> world_matrix{};
    AssetId mesh{};
};

class World {
public:
    [[nodiscard]] Entity create(Vec3 position = {});
    bool destroy(Entity entity);
    [[nodiscard]] bool alive(Entity entity) const;
    bool set_position(Entity entity, Vec3 position);
    // Returns false for an expired entity or a nonfinite/non-affine matrix.
    bool set_render_pose(Entity entity, const WorldMatrix& world_matrix, AssetId mesh);

    // Returns an owning copy in ascending slot order.
    [[nodiscard]] std::vector<RenderInstance> snapshot() const;

    // Replaces the contents with an owning copy, retaining the caller's capacity.
    void snapshot_into(std::vector<RenderInstance>& instances) const;

private:
    struct Slot {
        Vec3 position{};
        std::optional<WorldMatrix> world_matrix;
        AssetId mesh;
        std::uint32_t generation{1};
        bool occupied{};
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
};

} // namespace saige
