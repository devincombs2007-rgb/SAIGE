#include "saige/world.hpp"

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
    slot.position = position;
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

bool World::set_position(Entity entity, Vec3 position) {
    if (!alive(entity)) {
        return false;
    }
    slots_[entity.index].position = position;
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
        if (slot.occupied) {
            instances.push_back({{static_cast<std::uint32_t>(i), slot.generation}, slot.position});
        }
    }
}

} // namespace saige
