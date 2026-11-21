#include "saige/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {

RunStats run(World& world, IPlatform& platform, IGame& game, IFrameSink& sink,
             std::optional<std::uint64_t> frame_budget, RuntimeConfig config) {
    using namespace std::chrono;
    if (config.fixed_step <= nanoseconds::zero() || config.max_updates_per_frame == 0) {
        throw std::invalid_argument("SAIGE requires a positive fixed step and update limit");
    }

    RunStats stats;
    if (frame_budget == 0) {
        return stats;
    }
    auto previous = platform.now();
    auto remainder = nanoseconds::zero();
    while ((!frame_budget || stats.frames < *frame_budget) && platform.poll_events()) {
        const auto current = std::max(previous, platform.now());
        const auto elapsed = duration_cast<nanoseconds>(current - previous);
        previous = current;

        // Drop full ticks beyond the catch-up limit but retain the fractional
        // phase. Split the interval before adding to avoid accumulator overflow.
        auto updates = elapsed / config.fixed_step;
        const auto partial = elapsed % config.fixed_step;
        const auto until_tick = config.fixed_step - remainder;
        if (partial >= until_tick) {
            ++updates;
            remainder = partial - until_tick;
        } else {
            remainder += partial;
        }

        const auto limit = std::min<std::int64_t>(updates, config.max_updates_per_frame);
        for (std::int64_t i = 0; i < limit; ++i) {
            game.fixed_update(world, config.fixed_step);
            ++stats.ticks;
        }

        const auto instances = world.snapshot();
        const double phase = static_cast<double>(remainder.count()) /
                             static_cast<double>(config.fixed_step.count());
        sink.submit(FrameView{stats.ticks, std::min(phase, std::nextafter(1.0, 0.0)), instances});
        ++stats.frames;
    }
    return stats;
}

} // namespace saige
