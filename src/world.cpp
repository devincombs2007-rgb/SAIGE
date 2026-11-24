#include "saige/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace saige {

Entity World::create(Vec3 position) {
    std::uint32_t index;
    if (free_indices_.empty()) {
        if (slots_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("World entity index space exhausted");
        }
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    } else {
        index = free_indices_.back();
        free_indices_.pop_back();
    }
    auto& slot = slots_[index];
    slot.occupied = true;
    slot.visible = true;
    slot.position = position;
    slot.world_matrix.reset();
    slot.mesh = {};
    return {index, slot.generation};
}

bool World::alive(Entity entity) const {
    if (entity.index >= slots_.size()) {
        return false;
    }
    const auto& slot = slots_[entity.index];
    return slot.occupied && slot.generation == entity.generation;
}

bool World::destroy(Entity entity) {
    if (!alive(entity)) {
        return false;
    }
    auto& slot = slots_[entity.index];
    // Retire the slot at the generation limit so no stale handle can revive.
    if (slot.generation < std::numeric_limits<std::uint32_t>::max()) {
        free_indices_.push_back(entity.index);
        ++slot.generation;
    }
    slot.occupied = false;
    return true;
}

bool World::set_visible(Entity entity, bool visible) noexcept {
    if (!alive(entity)) return false;
    slots_[entity.index].visible = visible;
    return true;
}

bool World::set_position(Entity entity, Vec3 position) {
    if (!alive(entity)) {
        return false;
    }
    auto& slot = slots_[entity.index];
    if (slot.world_matrix) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            return false;
        }
        (*slot.world_matrix)[12] = position.x;
        (*slot.world_matrix)[13] = position.y;
        (*slot.world_matrix)[14] = position.z;
    }
    slot.position = position;
    return true;
}

bool World::set_render_pose(Entity entity, const WorldMatrix& matrix, AssetId mesh) {
    if (!alive(entity) || matrix[3] != 0.F || matrix[7] != 0.F || matrix[11] != 0.F || matrix[15] != 1.F ||
        !std::ranges::all_of(matrix, [](float value) { return std::isfinite(value); })) {
        return false;
    }
    auto& slot = slots_[entity.index];
    slot.position = {matrix[12], matrix[13], matrix[14]};
    slot.world_matrix = matrix;
    slot.mesh = mesh;
    return true;
}

std::vector<RenderInstance> World::snapshot() const {
    std::vector<RenderInstance> instances;
    snapshot_into(instances);
    return instances;
}

void World::snapshot_into(std::vector<RenderInstance>& instances) const {
    instances.clear();
    instances.reserve(slots_.size() - free_indices_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        if (slot.occupied && slot.visible) {
            instances.push_back({{static_cast<std::uint32_t>(i), slot.generation}, slot.position,
                                 slot.world_matrix, slot.mesh});
        }
    }
}

} // namespace saige
