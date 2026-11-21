#include "saige/world.hpp"

#include <iostream>

int main() {
    saige::World world;
    if (world.alive({}) || !world.snapshot().empty()) {
        std::cerr << "Expected an empty world at startup\n";
        return 1;
    }

    const saige::Vec3 start{1, 2, 3};
    const saige::Vec3 end{4, 5, 6};
    const auto original = world.create(start);
    const auto survivor = world.create();
    const auto before = world.snapshot();
    if (!world.set_position(survivor, end) || !world.destroy(original) ||
        world.destroy(original) || world.snapshot().size() != 1) {
        std::cerr << "Failed to update or destroy a live entity\n";
        return 1;
    }

    const auto replacement = world.create();
    if (replacement.index != original.index || replacement == original ||
        !world.alive(replacement) || world.alive(original) ||
        world.set_position(original, start) || world.destroy(original)) {
        std::cerr << "A stale handle interfered with a recycled slot\n";
        return 1;
    }

    const auto after = world.snapshot();
    if (before.size() != 2 || before[0].entity != original || before[0].position != start ||
        before[1].position != saige::Vec3{} || after.size() != 2 ||
        after[0].entity != replacement || after[0].position != saige::Vec3{} ||
        after[1].entity != survivor || after[1].position != end) {
        std::cerr << "Snapshot contents, lifetime, or ordering changed\n";
        return 1;
    }
}
