#include "saige/character_motor.hpp"
#include "saige/runtime.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace saige;
using namespace std::chrono_literals;

constexpr auto tick = 16'666'667ns;
constexpr std::array course{
    StaticBox{{-20, -20, -1}, {20, 20, 0}},
    StaticBox{{2, -3, 0}, {2.05f, 3, 2}},
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_contacts() {
    CharacterMotor motor({}, course);
    motor.step({}, tick);
    require(motor.state().grounded, "ground contact");
    for (int i = 0; i < 50; ++i) motor.step({1, 0, false, true}, tick);
    require(std::abs(motor.state().feet.x - 1.70f) < 0.0001f, "thin wall stops sprint");
    require(motor.state().velocity.x == 0 && motor.state().grounded, "wall contact clears the normal velocity");
    for (int i = 0; i < 18; ++i) motor.step({1, 1}, tick);
    require(motor.state().feet.y > 0.4f && std::abs(motor.state().feet.x - 1.70f) < 0.0001f, "slide along wall");

    motor.reset();
    motor.step({}, tick);
    motor.step({0, 0, true}, tick);
    require(!motor.state().grounded && motor.state().feet.z > 0, "jump leaves ground");
    for (int i = 0; i < 95; ++i) motor.step({0, 0, true}, tick);
    require(motor.state().grounded && motor.state().velocity.z == 0, "held jump must not bounce after landing");

    constexpr std::array ledge{StaticBox{{-1, -1, -1}, {1, 1, 0}}};
    CharacterMotor falling({}, ledge);
    for (int i = 0; i < 65; ++i) falling.step({1}, tick);
    require(falling.state().feet.z < 0 && !falling.state().grounded, "walking off the ledge loses contact");

    const auto before = motor.state();
    try {
        motor.step({std::numeric_limits<float>::quiet_NaN()}, tick);
        throw std::runtime_error("invalid input was accepted");
    } catch (const std::invalid_argument&) {
        require(motor.state() == before, "bad input cannot change state");
    }
}

void test_geometry_replacement() {
    CharacterMotor motor({}, course);
    require(motor.state().grounded, "spawn recognizes ground before the first tick");
    motor.step({0, 0, true}, tick);
    require(motor.state().feet.z > 0 && !motor.state().grounded, "jump works on the first tick");
    motor.reset();
    require(motor.state().grounded, "reset immediately restores ground contact");

    for (int i = 0; i < 45; ++i) motor.step({1, 0, false, true}, tick);
    const auto stopped = motor.state();
    constexpr std::array obstructed{
        course[0], StaticBox{{1, -1, 0}, {2, 1, 1}},
    };
    require(!motor.replace_static_geometry(obstructed) && motor.state() == stopped,
            "an overlapping candidate cannot replace active collision");
    constexpr std::array invalid{StaticBox{{1, 1, 1}, {0, 0, 0}}};
    try {
        (void)motor.replace_static_geometry(invalid);
        throw std::runtime_error("invalid collision was accepted");
    } catch (const std::invalid_argument&) {
        require(motor.state() == stopped, "invalid geometry cannot mutate the player");
    }
    motor.step({1}, tick);
    require(motor.state() == stopped, "old wall remains after both rejected replacements");

    require(motor.replace_static_geometry(std::span(course).first(1)), "accept an unobstructed replacement");
    for (int i = 0; i < 25; ++i) motor.step({1}, tick);
    require(motor.state().grounded && motor.state().feet.x > 2.1f, "retired wall no longer blocks movement");
    require(motor.replace_static_geometry({}) && !motor.state().grounded, "unloading floor removes support");
    motor.step({}, tick);
    require(motor.state().feet.z < stopped.feet.z, "unloaded floor no longer prevents gravity");
}

class ScriptClock final : public IPlatform {
public:
    std::chrono::steady_clock::time_point now() override { return time_; }
    bool poll_events() override {
        time_ += tick;
        return true;
    }
private:
    std::chrono::steady_clock::time_point time_{};
};

class ScriptGame final : public IGame {
public:
    explicit ScriptGame(World& world) : player_(world.create()) {}
    void fixed_update(World& world, std::chrono::nanoseconds dt) override {
        CharacterInput input;
        input.forward = frame_ < 90 ? 1.0f : -1.0f;
        input.left = frame_ >= 45 && frame_ < 90 ? 1.0f : 0.0f;
        input.jump = frame_ == 100;
        input.sprint = frame_ < 60;
        motor_.step(input, dt);
        require(world.set_position(player_, motor_.state().feet), "player entity kept its identity");
        states.push_back(motor_.state());
        ++frame_;
    }
    std::vector<CharacterState> states;
private:
    Entity player_;
    CharacterMotor motor_{{}, course};
    int frame_{};
};

class Probe final : public IFrameSink {
public:
    void submit(const FrameView& frame) override {
        require(frame.instances.size() == 1, "expected one character in each snapshot");
        poses.push_back(frame.instances.front().position);
    }
    std::vector<Vec3> poses;
};

void test_replay() {
    const auto play = [] {
        World world;
        ScriptClock platform;
        ScriptGame game(world);
        Probe probe;
        const auto stats = run(world, platform, game, probe, 180);
        require(stats.ticks == 180 && probe.poses.size() == 180, "replay has one tick per frame");
        return std::pair{game.states, probe.poses};
    };
    const auto first = play();
    require(first == play(), "recorded commands reproduce identical state and snapshots");
    require(first.first[101].feet.z > 0 && first.first.back().grounded, "replay includes a completed jump");
}
} // namespace

int main() {
    try {
        test_contacts();
        test_replay();
        test_geometry_replacement();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
