#pragma once

#include "saige/desktop.hpp"

namespace saige::sample {

struct MovementActions {
    float forward{};
    float left{};
};

[[nodiscard]] inline MovementActions read_movement(const IDesktopPlatform& platform) noexcept {
    const auto down = [&](Key wasd, Key arrow) { return platform.key_down(wasd) || platform.key_down(arrow); };
    return {static_cast<float>(down(Key::w, Key::up) - down(Key::s, Key::down)),
            static_cast<float>(down(Key::a, Key::left) - down(Key::d, Key::right))};
}

struct CourseActions {
    MovementActions movement;
    LookDelta look;
    bool jump{};
    bool sprint{};
    bool reset{};
};

[[nodiscard]] inline CourseActions read_course_actions(IDesktopPlatform& platform) noexcept {
    return {read_movement(platform), platform.consume_look_delta(), platform.key_down(Key::space),
            platform.key_down(Key::shift), platform.key_down(Key::r)};
}

} // namespace saige::sample
