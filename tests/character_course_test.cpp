#include "desktop_game.hpp"
#include "saige/capsule_character.hpp"
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
    Aabb{{-20, -20, -1}, {20, 20, 0}},
    Aabb{{2, -3, 0}, {2.05f, 3, 2}},
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_contacts() {
    CapsuleCharacter motor({}, course);
    motor.step({}, tick);
    require(motor.state().grounded, "ground contact");
    for (int i = 0; i < 50; ++i) motor.step({1, 0, false, true}, tick);
    require(std::abs(motor.state().feet.x - 1.70f) < 0.0002f, "thin wall stops sprint");
    require(motor.state().velocity.x == 0 && motor.state().grounded, "wall contact clears the normal velocity");
    for (int i = 0; i < 18; ++i) motor.step({1, 1}, tick);
    require(motor.state().feet.y > 0.4f && std::abs(motor.state().feet.x - 1.70f) < 0.0002f, "slide along wall");

    motor.reset();
    motor.step({}, tick);
    motor.step({0, 0, true}, tick);
    require(!motor.state().grounded && motor.state().feet.z > 0, "jump leaves ground");
    for (int i = 0; i < 95; ++i) motor.step({0, 0, true}, tick);
    require(motor.state().grounded && motor.state().velocity.z == 0, "held jump must not bounce after landing");

    constexpr std::array ledge{Aabb{{-1, -1, -1}, {1, 1, 0}}};
    CapsuleCharacter falling({}, ledge);
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
    CapsuleCharacter motor({}, course);
    require(motor.state().grounded, "spawn recognizes ground before the first tick");
    motor.step({0, 0, true}, tick);
    require(motor.state().feet.z > 0 && !motor.state().grounded, "jump works on the first tick");
    motor.reset();
    require(motor.state().grounded, "reset immediately restores ground contact");

    for (int i = 0; i < 45; ++i) motor.step({1, 0, false, true}, tick);
    const auto stopped = motor.state();
    constexpr std::array obstructed{
        course[0], Aabb{{1, -1, 0}, {2, 1, 1}},
    };
    require(!motor.replace_static_geometry(obstructed) && motor.state() == stopped,
            "an overlapping candidate cannot replace active collision");
    constexpr std::array invalid{Aabb{{1, 1, 1}, {0, 0, 0}}};
    try {
        (void)motor.replace_static_geometry(invalid);
        throw std::runtime_error("invalid collision was accepted");
    } catch (const std::invalid_argument&) {
        require(motor.state() == stopped, "invalid geometry cannot mutate the player");
    }
    motor.step({1}, tick);
    require(std::abs(motor.state().feet.x - stopped.feet.x) < 0.0002f && motor.state().grounded,
            "old wall remains after both rejected replacements");

    require(motor.replace_static_geometry(std::span(course).first(1)), "accept an unobstructed replacement");
    for (int i = 0; i < 25; ++i) motor.step({1}, tick);
    require(motor.state().grounded && motor.state().feet.x > 2.1f, "retired wall no longer blocks movement");
    require(motor.replace_static_geometry({}) && !motor.state().grounded, "unloading floor removes support");
    motor.step({}, tick);
    require(motor.state().feet.z < stopped.feet.z, "unloaded floor no longer prevents gravity");
}

class CourseInput final : public IDesktopPlatform {
public:
    std::chrono::steady_clock::time_point now() override { return {}; }
    bool poll_events() override { return true; }
    NativeWindowHandle native_window() const noexcept override { return {}; }
    SurfaceState surface() const noexcept override { return {}; }
    bool key_down(Key key) const noexcept override {
        return (key == Key::w && forward) || (key == Key::space && jump) || (key == Key::r && reset);
    }
    LookDelta consume_look_delta() noexcept override { return std::exchange(look, {}); }

    LookDelta look{};
    bool forward{}, jump{}, reset{};
};

void test_desktop_course() {
    World world;
    CourseInput input;
    sample::DesktopGame game(world, input, true);
    const auto player = [&] { return world.snapshot().front().position; };
    const auto advance = [&] { game.fixed_update(world, tick); };
    const auto spawn_camera = game.camera_view().value();
    const auto camera_position = [&] { return game.camera_view().value().position; };

    input.look = {-524.0f, 60.0f}; // About a quarter-turn left, plus down.
    advance();
    const auto turned = camera_position();
    require(turned.y < -4.9f && turned.z > spawn_camera.position.z, "look orbits in yaw and pitch");
    advance();
    require(camera_position() == turned, "a fixed update must consume mouse input only once");

    input.forward = true;
    for (int i = 0; i < 30; ++i) advance();
    const auto moved = player();
    require(std::abs(moved.x) < 0.01f && moved.y > 1.0f, "forward follows the orbit heading");
    require(std::abs(camera_position().y - turned.y - moved.y) < 0.001f, "camera follows the player");

    input.forward = false;
    input.jump = true;
    advance();
    require(player().z > 0.02f, "course jump works");
    input.reset = true;
    input.look = {100.0f, 100.0f};
    advance();
    require(player() == Vec3{} && camera_position() == spawn_camera.position, "reset restores player and orbit");
    input.reset = false;
    advance();
    require(player().z < 0.001f && camera_position().y == spawn_camera.position.y,
            "held jump and discarded drag must not replay after reset");
    input.jump = false;
    advance();
    input.jump = true;
    advance();
    require(player().z > 0.02f, "released jump can be pressed again");

    input.jump = false;
    input.look = {-524.0f, 0.0f};
    input.forward = true;
    bool fell = false;
    bool recovered = false;
    for (int i = 0; i < 900; ++i) {
        advance();
        fell = fell || player().z < -0.1f;
        if (fell && player() == Vec3{}) {
            recovered = camera_position() == spawn_camera.position;
            break;
        }
    }
    require(fell && recovered, "walking off the floor restores the spawn and camera");

    World preview_world;
    sample::DesktopGame preview(preview_world, input, false);
    preview.fixed_update(preview_world, tick);
    require(!preview.camera_view() && preview_world.snapshot().size() == 1,
            "default renderer preview retains its original camera and marker");
}

void test_obstructed_camera_and_speeds() {
    World world;
    CourseInput input;
    CapsuleCharacterConfig config;
    config.walk_speed = 1.5f;
    config.sprint_speed = 4.0f;
    sample::DesktopGame game(world, input, true, config);
    const auto advance = [&] { game.fixed_update(world, tick); };
    const auto player_x = [&] { return world.snapshot().front().position.x; };
    input.forward = true;
    for (int i = 0; i < 40; ++i) advance();
    require(player_x() > 0.8f && player_x() < 1.05f, "custom walk speed applies to gameplay");
    for (int i = 0; i < 70; ++i) advance();
    require(player_x() > 2.18f && player_x() < 2.21f, "the same capsule reaches the obstacle");
    input.forward = false;
    input.look = {1047.0f, -100.0f};
    advance();
    const auto obstructed = game.camera_view().value().position;
    require(obstructed.x > player_x() && obstructed.x < 2.4f, "camera retracts before the obstacle");
    input.look = {-1047.0f, 0.0f};
    advance();
    const auto returning = game.camera_view().value().position;
    require(player_x() - returning.x < 1.0f, "camera extension is rate limited");
    for (int i = 0; i < 35; ++i) advance();
    require(player_x() - game.camera_view().value().position.x > 5.8f, "camera restores the unobstructed orbit");
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
        CapsuleInput input;
        input.forward = frame_ < 90 ? 1.0f : -1.0f;
        input.left = frame_ >= 45 && frame_ < 90 ? 1.0f : 0.0f;
        input.jump = frame_ == 100;
        input.sprint = frame_ < 60;
        motor_.step(input, dt);
        require(world.set_position(player_, motor_.state().feet), "player entity kept its identity");
        states.push_back(motor_.state());
        ++frame_;
    }
    std::vector<CapsuleCharacterState> states;
private:
    Entity player_;
    CapsuleCharacter motor_{{}, course};
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
        test_desktop_course();
        test_obstructed_camera_and_speeds();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
