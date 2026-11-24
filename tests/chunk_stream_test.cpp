#include <thread>
#include "streamed_course.hpp"
#include "saige/capsule_character.hpp"
#include "saige/chunk_manifest.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using saige::ChunkCoord;
using saige::ChunkSource;
using saige::ChunkStream;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void test_stream(const std::filesystem::path& fixture) {
    const ChunkSource west{{0, 0}, fixture};
    const ChunkSource east{{1, 0}, fixture};
    const saige::PreparedScene reference(fixture);
    const auto bytes = reference.asset_payload_bytes();
    const auto mesh = reference.assets().front().id;
    ChunkStream stream({2, bytes * 2});

    stream.request(std::array{east, west});
    require(!stream.commit_frame(), "incomplete batch must remain invisible");
    require(stream.stage_next() && stream.retained_asset_bytes() == bytes, "one preparation per step");
    require(!stream.commit_frame(), "one of two chunks must remain invisible");
    require(!stream.ready_to_commit() && stream.staged_chunk(west.coord) && !stream.staged_chunk(east.coord),
            "a partial candidate can be read but is not ready");
    require(stream.stage_next() && stream.commit_frame(), "both chunks should become active together");
    const auto* west_scene = stream.active_chunk(west.coord);
    const auto* east_scene = stream.active_chunk(east.coord);
    require(west_scene && east_scene && west_scene->find_asset(mesh) && east_scene->find_asset(mesh), "stable asset identity");

    stream.request(std::array{east});
    require(!stream.stage_next() && stream.commit_frame(), "crossing into the east chunk reuses its payload");
    require(stream.active_count() == 1 && stream.active_chunk(east.coord) == east_scene &&
            stream.retained_asset_bytes() == bytes, "eviction releases the west payload");

    const ChunkSource missing{{0, 0}, fixture.parent_path() / "missing.scene.json"};
    stream.request(std::array{east, missing});
    bool failed = false;
    try {
        stream.stage_next();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed && !stream.commit_frame() && stream.active_chunk(east.coord) == east_scene, "failed request retains active set");

    stream.request(std::array{east, west});
    require(stream.stage_next() && stream.retained_asset_bytes() == bytes * 2, "revisit prepares the missing chunk");
    stream.cancel();
    require(!stream.commit_frame() && !stream.active_chunk(west.coord) && stream.retained_asset_bytes() == bytes, "cancel releases staged payload");
    stream.request(std::array{west, east});
    require(stream.stage_next() && stream.ready_to_commit(), "retry prepares the complete candidate");
    require(stream.staged_chunk(west.coord)->find_asset(mesh) && stream.staged_chunk(east.coord) == east_scene,
            "the candidate exposes staged and reused assets before publication");
    stream.cancel(); // A collision consumer can reject the complete candidate.
    require(!stream.ready_to_commit() && !stream.staged_chunk(west.coord) && stream.active_chunk(east.coord) == east_scene,
            "a rejected candidate keeps the previous active set");
    stream.request(std::array{west, east});
    require(stream.stage_next() && stream.commit_frame() && stream.active_chunk(west.coord)->find_asset(mesh), "retry preserves stable mesh ID");

    ChunkStream moving({2, bytes * 2});
    moving.request(std::array{east, west});
    require(moving.stage_next(), "prepare the first of two requested chunks");
    moving.request(std::array{west, east});
    require(moving.retained_asset_bytes() == bytes, "repeated requests keep completed staging");
    require(moving.stage_next() && moving.commit_frame(), "moving requests don't starve preparation");
    moving.request(std::array{west});
    require(moving.commit_frame(), "evict east to leave preparation headroom");
    const ChunkSource north{{0, 1}, fixture};
    moving.request(std::array{west, east});
    require(moving.stage_next() && moving.retained_asset_bytes() == bytes * 2, "prepare candidate east");
    moving.request(std::array{west, north});
    require(moving.retained_asset_bytes() == bytes, "superseded chunks immediately release retained payload");
    require(moving.stage_next() && moving.commit_frame() && moving.active_chunk(north.coord) &&
            !moving.active_chunk(east.coord), "supersession activates only the newly desired chunks");

    stream.request(std::array{east, missing});
    stream.request(std::array{west});
    require(!stream.stage_next() && stream.commit_frame() && !stream.active_chunk(east.coord),
            "a superseded request must not resurrect an evicted chunk");
    try {
        stream.request(std::array{west, west});
        throw std::runtime_error("duplicate coordinates were accepted");
    } catch (const std::invalid_argument&) {
        require(stream.active_count() == 1 && !stream.commit_frame(), "invalid request must leave residency intact");
    }

    ChunkStream limited({2, bytes});
    limited.request(std::array{east, missing});
    failed = false;
    try {
        limited.stage_next(); // Lower coordinate goes first despite request order.
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed && limited.retained_asset_bytes() == 0, "deterministic missing chunk fails before east");
    limited.request(std::array{west, east});
    require(limited.stage_next(), "first chunk fits");
    failed = false;
    try {
        limited.stage_next();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed && !limited.commit_frame() && limited.retained_asset_bytes() == 0, "aggregate budget abandons the candidate");
    stream.request({});
    require(stream.commit_frame() && stream.active_count() == 0 && stream.retained_asset_bytes() == 0, "empty desired set unloads all chunks");
}

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("saige-chunk-test-" + std::to_string(std::random_device{}()));
    Workspace() { require(fs::create_directory(root), "create chunk workspace"); }
    ~Workspace() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void write_chunk_fixture(const fs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    require(file.good(), "write chunk fixture");
}

void test_background_stage(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    const auto missing = workspace.root / "missing.json";
    const ChunkSource origin{{0, 0}, fixture};
    const ChunkSource invalid{{1, 0}, fixture, missing};
    const ChunkSource valid{{2, 0}, fixture};
    ChunkStream stream;
    stream.request(std::array{origin});
    require(stream.stage_next() && stream.commit_frame(), "bootstrap the active scene");
    const auto original = stream.active_chunk(origin.coord);

    const auto poll = [&](auto complete) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!complete() && std::chrono::steady_clock::now() < deadline) {
            stream.stage_background();
            std::this_thread::sleep_for(1ms);
        }
        require(complete(), "background preparation did not complete");
    };
    stream.request(std::array{origin, invalid});
    stream.stage_background();
    stream.cancel();
    stream.request(std::array{origin, valid});
    require(!stream.commit_frame() && stream.active_chunk(origin.coord) == original,
            "a background request cannot change resident state before publication");
    poll([&] { return stream.ready_to_commit(); });
    require(stream.commit_frame() && stream.active_chunk(origin.coord) == original &&
                stream.active_chunk(valid.coord) && !stream.active_chunk(invalid.coord),
            "a superseded failure cannot replace or cancel the new request");

    stream.request(std::array{origin, invalid});
    bool failed = false;
    try {
        poll([&] { return stream.ready_to_commit(); });
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()).find("open chunk file") != std::string_view::npos;
    }
    require(failed && !stream.commit_frame() && stream.active_chunk(valid.coord),
            "a current failure rolls back to the previous active set");
}

void test_shared_project_preparation(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    const auto old_collision = workspace.root / "west.json";
    const auto new_collision = workspace.root / "east.json";
    const ChunkSource west{{0, 0}, fixture, old_collision};
    const ChunkSource east{{1, 0}, fixture, new_collision};
    write_chunk_fixture(old_collision, R"({"schema_version":1,"boxes":[]})");
    const auto bytes = saige::PreparedScene(fixture).asset_payload_bytes();
    ChunkStream stream({.max_chunks = 2, .max_retained_asset_bytes = bytes, .share_immutable_projects = true});
    stream.request(std::array{west});
    require(stream.stage_next() && stream.commit_frame(), "prepare one scene at the full byte budget");
    const auto* shared = stream.active_chunk(west.coord);

    stream.request(std::array{west, east});
    bool failed = false;
    try {
        stream.stage_next();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && !stream.ready_to_commit() && stream.active_chunk(west.coord) == shared,
            "shared CPU preparation cannot bypass a missing collision file");

    write_chunk_fixture(new_collision, R"({"schema_version":1,"boxes":[{"min":[0,0,-1],"max":[8,8,0]}]})");
    stream.request(std::array{west, east});
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!stream.ready_to_commit() && std::chrono::steady_clock::now() < deadline) {
        stream.stage_background();
        std::this_thread::sleep_for(1ms);
    }
    require(stream.ready_to_commit() && stream.staged_chunk(east.coord) == shared && stream.retained_asset_bytes() == bytes,
            "background preparation shares one immutable CPU scene at the exact budget");
    require(stream.staged_collision(west.coord)->empty() && stream.staged_collision(east.coord)->size() == 1,
            "each chunk owns its authored collision");
    require(stream.commit_frame(), "publish shared scene");
    stream.request(std::array{east});
    require(stream.commit_frame() && stream.active_chunk(east.coord) == shared && stream.retained_asset_bytes() == bytes,
            "the new cell retains the scene after eviction of its original owner");
    stream.request({});
    require(stream.commit_frame() && stream.retained_asset_bytes() == 0,
            "evict the prepared scene when its final reference is released");
}

void test_collision_handoff(const std::filesystem::path& fixture) {
    using namespace std::chrono_literals;
    constexpr auto tick = 16'666'667ns;
    Workspace workspace;
    const auto west_file = workspace.root / "west.collision.json";
    const auto east_file = workspace.root / "east.collision.json";
    const ChunkSource west{{-1, 0}, fixture, west_file};
    const ChunkSource east{{0, 0}, fixture, east_file};
    constexpr std::string_view valid_east = R"({"schema_version":1,"boxes":[{"min":[0,-4,-1],"max":[8,4,0]}]})";
    write_chunk_fixture(west_file, R"({"schema_version":1,"boxes":[{"min":[-8,-4,-1],"max":[0,4,0]}]})");
    ChunkStream stream;
    stream.request(std::array{west});
    require(stream.stage_next() && stream.ready_to_commit(), "stage the initial west chunk");
    saige::CapsuleCharacter player({-0.8f, 0, 0}, stream.staged_collision(west.coord).value());
    require(stream.commit_frame(), "publish the initial west chunk");
    const auto* resident_west = stream.active_chunk(west.coord);

    stream.request(std::array{east, west});
    require(!stream.ready_to_commit() && !stream.commit_frame() && !stream.staged_collision(east.coord),
            "unprepared neighbor cannot publish");
    const auto original_player = player.state();
    bool missing = false;
    try {
        (void)stream.stage_next();
    } catch (const std::runtime_error&) {
        missing = true;
    }
    require(missing && !stream.ready_to_commit() && !stream.staged_collision(west.coord) &&
                player.state() == original_player && stream.active_chunk(west.coord) == resident_west,
            "a missing collision file abandons staging and preserves the active course");

    const ChunkSource changed_west{west.coord, fixture, east_file};
    stream.request(std::array{changed_west});
    require(!stream.ready_to_commit() && !stream.staged_collision(west.coord),
            "changing only the collision path invalidates reuse for that coordinate");
    stream.cancel();

    for (const auto invalid : {
        R"({"schema_version":2,"boxes":[]})",
        R"({"schema_version":1,"boxes":[{"min":[0,0,0],"max":[0,1,1]}]})",
        R"({"schema_version":1,"boxes":[{"min":[0,0,0],"max":[1e40,1,1]}]})",
        R"({"schema_version":1,"boxes":[{"min":[0,0,"bad"],"max":[1,1,1]}]})",
        R"({"schema_version":1,"boxes":[{"min":[[[[[[[[[[]]]]]]]]]],"max":[1,1,1]}]})",
    }) {
        write_chunk_fixture(east_file, invalid);
        stream.request(std::array{east, west});
        bool rejected = false;
        try {
            (void)stream.stage_next();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && !stream.ready_to_commit(), "invalid collision file is rejected");
    }
    require(player.state() == original_player && stream.active_chunk(west.coord) == resident_west &&
                !stream.active_chunk(east.coord) && !stream.commit_frame(),
            "invalid collision discards the pending set without changing the active one");

    write_chunk_fixture(east_file, R"({"schema_version":1,"boxes":[
        {"min":[0,-4,-1],"max":[8,4,0]},
        {"min":[-1.5,-1,0],"max":[0,1,2]}
    ]})");
    const auto staged_geometry = [&] {
        const auto west_boxes = stream.staged_collision(west.coord).value();
        const auto east_boxes = stream.staged_collision(east.coord).value();
        std::vector<saige::Aabb> joined(west_boxes.begin(), west_boxes.end());
        joined.insert(joined.end(), east_boxes.begin(), east_boxes.end());
        return joined;
    };
    stream.request(std::array{west, east});
    require(stream.stage_next() && stream.ready_to_commit() && stream.staged_chunk(west.coord) == resident_west,
            "candidate reuses the resident west and stages the east");
    const auto blocked = staged_geometry();
    require(!player.replace_static_geometry(blocked), "candidate collision overlapping the player must be rejected");
    stream.cancel();
    require(player.state() == original_player && stream.active_chunk(west.coord) == resident_west &&
                !stream.active_chunk(east.coord) && !stream.commit_frame(),
            "rejecting collision preserves both player state and residency");

    write_chunk_fixture(east_file, valid_east);
    stream.request(std::array{west, east});
    require(stream.stage_next() && stream.ready_to_commit(), "retry after correcting the collision file");
    const auto joined = staged_geometry();
    require(player.replace_static_geometry(joined) && stream.commit_frame(),
            "publish accepted collision and residency at the same simulation boundary");
    const auto* resident_east = stream.active_chunk(east.coord);
    require(resident_east, "publish the eastern neighbor");
    for (int step = 0; step < 40; ++step) {
        player.step({1.0f}, tick);
        require(player.state().grounded, "player must stay supported crossing the chunk seam");
    }
    require(player.state().feet.x > 1.0f, "player reaches the east chunk");

    stream.request(std::array{east});
    require(stream.ready_to_commit() && stream.staged_chunk(east.coord) == resident_east,
            "retiring west reuses the east payload without staging");
    require(player.replace_static_geometry(stream.staged_collision(east.coord).value()) && stream.commit_frame(),
            "retire west collision when the player is supported by east");
    require(!stream.active_chunk(west.coord) && stream.active_chunk(east.coord) == resident_east &&
                player.state().grounded,
            "retiring a chunk preserves support and identity in the retained neighbor");

    // Once the original spawn has unloaded, the host must choose a resident reset point.
    // The handoff itself must not silently clamp the live character to the old chunk.
    for (int step = 0; step < 140 && player.state().feet.z > -0.1f; ++step) player.step({-1.0f}, tick);
    require(player.state().feet.x < 0.0f && player.state().feet.z <= -0.1f && !player.state().grounded,
            "the retired west floor can no longer support the player");
}

class CourseKeys final : public saige::IDesktopPlatform {
public:
    std::chrono::steady_clock::time_point now() override { return {}; }
    bool poll_events() override { return true; }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key key) const noexcept override {
        return (key == saige::Key::w && forward) || (key == saige::Key::s && back) || (key == saige::Key::r && reset) || (key == saige::Key::d && right) ||
               (key == saige::Key::space && brake) || (key == saige::Key::e && interact) || (key == saige::Key::c && checkpoint);
    }
    bool forward{};
    bool back{};
    bool reset{};
    bool right{};
    bool brake{};
    bool interact{};
    bool checkpoint{};
};

class CourseOutputProbe final : public saige::sample::CourseSceneOutput {
public:
    bool stage(std::span<const saige::sample::CourseAsset> assets) override {
        require(assets.size() == 1 && assets.front().model, "shared assets stage once per set");
        if (fail) throw std::runtime_error("forced GPU staging failure");
        staged = true;
        return true;
    }
    void publish() noexcept override {
        if (staged) ++publications;
        staged = false;
    }
    void discard() noexcept override { staged = false; }
    bool fail{};
    bool staged{};
    int publications{};
};

void test_native_streamed_course(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    fs::copy(fixture.parent_path(), workspace.root / "assets", fs::copy_options::recursive);
    const auto manifest = workspace.root / "course.json";
    const auto east = workspace.root / "east.json";
    write_chunk_fixture(manifest, R"({"schema_version":1,"chunks":[
        {"coord":[1,0],"project":"assets/saige.scene.json","collision":"east.json"},
        {"coord":[2,0],"project":"assets/saige.scene.json","collision":"next.json"},
        {"coord":[3,0],"project":"assets/saige.scene.json","collision":"last.json"},
        {"coord":[0,0],"project":"assets/saige.scene.json","collision":"origin.json"}
    ]})");
    write_chunk_fixture(workspace.root / "origin.json",
        R"({"schema_version":1,"boxes":[{"min":[0,0,-1],"max":[8,8,0]}]})");

    write_chunk_fixture(workspace.root / "next.json",
        R"({"schema_version":1,"boxes":[{"min":[16,0,-1],"max":[24,8,0]}]})");
    write_chunk_fixture(workspace.root / "last.json",
        R"({"schema_version":1,"boxes":[{"min":[24,0,-1],"max":[32,8,0]}]})");

    saige::World world;
    CourseKeys keys;
    CourseOutputProbe output;
    saige::sample::StreamedCourse course(world, keys, manifest, &output);
    const auto player = [&] { return world.snapshot().front().position; };
    const auto original = world.snapshot();
    require(original.size() == 4 && output.publications == 1, "initial scene is published with the reset chunk");
    const auto tick = [&] { course.fixed_update(world, 16'666'667ns); };
    const saige::Vec3 spawn{4, 4, 0};
    require(course.resident_count() == 1 && player() == spawn, "start with a supported resident reset point");
    tick(); // east is missing
    keys.forward = true;
    for (int frame = 0; frame < 150; ++frame) tick();
    require(course.resident_count() == 1 && player().x > 7.6f && player().x < 7.71f && player().z >= 0,
            "failed staging keeps the old boundary and prevents walking into the missing cell");

    write_chunk_fixture(east, R"({"schema_version":1,"boxes":[
        {"min":[8,0,-1],"max":[16,8,0]}, {"min":[3,3,0],"max":[5,5,2]}
    ]})");
    keys.reset = true;
    tick();
    require(course.resident_count() == 1 && player() == spawn, "reject new geometry overlapping the reset point");
    keys.reset = false;
    tick();

    write_chunk_fixture(east, R"({"schema_version":1,"boxes":[{"min":[8,0,-1],"max":[16,8,241]}]})");
    keys.reset = true;
    tick();
    require(course.resident_count() == 1 && player() == spawn, "reject collision above the supported height");
    keys.reset = false;
    tick();

    write_chunk_fixture(east, R"({"schema_version":1,"boxes":[{"min":[8,0,-1],"max":[16,8,0]}]})");
    output.fail = true;
    keys.reset = true;
    tick();
    require(course.resident_count() == 1 && player() == spawn && output.publications == 1 && !output.staged &&
                world.snapshot().size() == original.size(),
            "GPU staging failure preserves the published collision, residency and render world");
    keys.reset = false;
    tick();

    output.fail = false;
    keys.reset = true;
    tick();
    require(course.resident_count() == 2 && player() == spawn && output.publications == 2,
            "retry publishes resident collision and GPU models at the tick boundary");
    const auto composed = world.snapshot();
    require(composed.size() == 7 && composed[1].entity == original[1].entity &&
                composed[4].position.x == original[1].position.x + 8.0f && composed[4].mesh == original[1].mesh,
            "resident identity, cell translation and shared asset identity survive composition");
    keys.reset = false;
    for (int frame = 0; frame < 140; ++frame) tick();
    require(player().x > 8 && player().z >= 0, "player walks across the streamed seam");
    for (int frame = 0; frame < 460; ++frame) tick();
    require(player().x > 25 && player().z >= 0 && course.resident_count() == 3,
            "the course evicts an intermediate chunk while retaining reset and current neighbors");
    keys.forward = false;
    keys.back = true;
    for (int frame = 0; frame < 330; ++frame) tick();
    require(player().x < 16 && player().z >= 0, "reverse traversal revisits retired ground");
    for (int frame = 0; frame < 48; ++frame) {
        keys.forward = frame % 2 == 0;
        keys.back = !keys.forward;
        tick();
        require(player().z >= 0, "rapid direction changes preserve contact");
    }
    keys.forward = false;
    keys.back = false;
    keys.reset = true;
    tick();
    require(player() == spawn && course.resident_count() == 2, "reset remains resident across streaming");
    require(course.camera_view().has_value(), "streamed course retains the existing orbit camera");
}

constexpr std::int32_t long_course_start = std::numeric_limits<std::int32_t>::min();

fs::path write_long_course(const fs::path& fixture, const Workspace& workspace, int count) {
    fs::copy(fixture.parent_path(), workspace.root / "assets", fs::copy_options::recursive);
    const auto manifest = workspace.root / "long-course.json";
    std::string json = R"({"schema_version":2,"origin":[-2147483648,0],"checkpoint":[4,4,0],"chunks":[)";
    for (int i = 0; i < count; ++i) {
        if (i != 0) json += ',';
        json += R"({"coord":[)" + std::to_string(long_course_start + i) +
                R"(,0],"project":"assets/saige.scene.json","collision":"floor.json"})";
    }
    write_chunk_fixture(manifest, json + "]}");
    write_chunk_fixture(workspace.root / "floor.json",
        R"({"schema_version":1,"boxes":[{"min":[0,0,-1],"max":[8,8,0]}]})");

    return manifest;
}

void test_live_origin_migration(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    const auto manifest = write_long_course(fixture, workspace, 12);
    constexpr auto first_cell = long_course_start;
    saige::World world;
    CourseKeys keys;
    CourseOutputProbe output;
    saige::sample::StreamedCourse course(world, keys, manifest, &output);
    const auto initial = world.snapshot();
    const auto tick = [&] { course.fixed_update(world, 16'666'667ns); };
    const auto absolute_x = [&](float local_x) {
        const float within_cell = local_x - std::floor(local_x / 8.0f) * 8.0f;
        return static_cast<float>(course.absolute_cell().first - first_cell) * 8.0f + within_cell;
    };

    keys.forward = true;
    float previous_x = 4.0f;
    for (int i = 0; i < 1450; ++i) {
        tick();
        const auto position = world.snapshot().front().position;
        const float x = absolute_x(position.x);
        require(x >= previous_x && x - previous_x < 0.15f && position.z >= 0,
                "walking remains continuous through streamed transitions and coordinate changes");
        previous_x = x;
    }
    require(previous_x > 75 && course.origin_epoch() >= 2, "the course walks through repeated live origin shifts");
    const auto shifted = world.snapshot();
    const auto checkpoint = std::ranges::find(shifted, initial[1].entity, &saige::RenderInstance::entity);
    require(checkpoint != shifted.end() && checkpoint->position.x < initial[1].position.x - 60,
            "the pinned render entity keeps its identity and translates into the new frame");

    const auto epoch = course.origin_epoch();
    keys.forward = false;
    keys.reset = true;
    tick();
    require(course.absolute_cell().first == first_cell, "reset immediately returns to the original logical cell");
    for (int i = 0; i < 5; ++i) tick();
    require(world.snapshot().front().position == saige::Vec3{4, 4, 0} && course.origin_epoch() > epoch,
            "the retained checkpoint and render data return to their starting frame");
}

void test_persistent_checkpoint(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    const auto manifest = write_long_course(fixture, workspace, 88);
    const auto file = workspace.root / "checkpoint.json";
    saige::World world;
    CourseKeys keys;
    CourseOutputProbe output;
    saige::sample::StreamedCourse course(world, keys, manifest, &output, {}, false, false, file);
    const auto tick = [&] { course.fixed_update(world, 16'666'667ns); };
    const auto save = [&] {
        keys.checkpoint = true;
        tick();
        keys.checkpoint = false;
        tick();
    };

    // Force a filesystem failure after construction. Reset must still return to the authored point.
    fs::create_directory(file);
    keys.forward = true;
    for (int frame = 0; frame < 48; ++frame) tick();
    keys.forward = false;
    save();
    keys.reset = true;
    tick();
    keys.reset = false;
    require(world.snapshot().front().position == saige::Vec3{4, 4, 0} && fs::is_directory(file),
            "a failed file replacement cannot move the live reset point");
    fs::remove(file);
    save(); // Create the initial valid file so later saves exercise replacement.

    const auto starting_entity = world.snapshot()[1].entity;
    keys.forward = true;
    for (int target : {18, 36, 54, 72}) {
        for (int frame = 0; frame < 3000; ++frame) {
            const auto local = world.snapshot().front().position.x;
            const float within_cell = local - std::floor(local / 8.0f) * 8.0f;
            if (course.absolute_cell().first == long_course_start + target && within_cell >= 3.0f) break;
            tick();
        }
        require(course.absolute_cell().first == long_course_start + target, "traverse beyond the original origin window");
        save();
        const auto saved = saige::load_course_checkpoint(file);
        require(saved.cell.x == long_course_start + target && saved.feet.x > 2 && saved.feet.x < 4,
                "save the absolute cell and bounded local coordinates");
    }
    const auto active = world.snapshot();
    require(std::ranges::find(active, starting_entity, &saige::RenderInstance::entity) == active.end(),
            "the original spawn chunk and its render entities can be evicted");

    keys.forward = false;
    const auto saved = saige::load_course_checkpoint(file);
    keys.brake = true;
    tick(); // Jump.
    save();
    require(saige::load_course_checkpoint(file).feet == saved.feet, "airborne saves preserve the last grounded checkpoint");
    keys.brake = false;
    keys.reset = true;
    tick();
    require(course.absolute_cell().first == saved.cell.x, "reset restores the last saved cell");

    saige::World reopened;
    CourseKeys reopened_keys;
    saige::sample::StreamedCourse resumed(reopened, reopened_keys, manifest, nullptr, {}, false, false, file);
    require(resumed.absolute_cell().first == saved.cell.x && reopened.snapshot().front().position == saved.feet,
            "relaunch restores the same authored-world position independently of the previous origin");

    write_chunk_fixture(file, R"({"checkpoint_version":1,"cell":[0,0],"feet":[3,4,0]})");
    saige::World invalid_world;
    bool rejected = false;
    try {
        saige::sample::StreamedCourse invalid(invalid_world, reopened_keys, manifest, nullptr, {}, false, false, file);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && invalid_world.snapshot().empty(), "a checkpoint outside the manifest must fail before world publication");
}

void test_streamed_vehicle(const fs::path& fixture) {
    using namespace std::chrono_literals;
    Workspace workspace;
    fs::copy(fixture.parent_path(), workspace.root / "assets", fs::copy_options::recursive);
    const auto manifest = workspace.root / "vehicle.json";
    const auto east = workspace.root / "east.json";
    write_chunk_fixture(manifest, R"({"schema_version":2,"origin":[-2147483648,0],"checkpoint":[4,4,0],"chunks":[
        {"coord":[-2147483648,0],"project":"assets/saige.scene.json","collision":"floor.json"},
        {"coord":[-2147483647,0],"project":"assets/saige.scene.json","collision":"east.json"},
        {"coord":[-2147483646,0],"project":"assets/saige.scene.json","collision":"floor.json"},
        {"coord":[-2147483645,0],"project":"assets/saige.scene.json","collision":"floor.json"}
    ]})");
    constexpr std::string_view floor = R"({"schema_version":1,"boxes":[{"min":[0,0,-1],"max":[8,8,0]}]})";
    write_chunk_fixture(workspace.root / "floor.json", floor);

    saige::World world;
    CourseKeys keys;
    saige::sample::StreamedCourse course(world, keys, manifest, nullptr, {}, true);
    const auto tick = [&] { course.fixed_update(world, 16'666'667ns); };
    const auto player = [&] { return world.snapshot().front().position; };
    const auto car = [&] { return world.snapshot().back().position; };
    const auto enter = [&] {
        keys.right = true;
        for (int i = 0; i < 49; ++i) tick();
        keys.right = false;
        keys.interact = true;
        tick();
        require(course.driving(), "enter a supported streamed vehicle");
        keys.interact = false;
    };
    enter();
    keys.forward = true;
    for (int i = 0; i < 160; ++i) tick();
    require(course.resident_count() == 1 && car().x < 7.41f && car().x > 7.2f,
            "vehicle stops before the missing chunk and leaves the rider seated");

    // Repair the chunk, reset the pair and retry the existing stream request.
    write_chunk_fixture(east, floor);
    keys.forward = false;
    keys.reset = true;
    tick();
    keys.reset = false;
    require(!course.driving() && player() == saige::Vec3{4, 4, 0} && car() == saige::Vec3{4, 1, 0},
            "reset returns the rider and vehicle to their supported positions");
    enter();
    keys.forward = true;
    for (int i = 0; i < 100; ++i) tick();
    keys.forward = false;
    keys.brake = true;
    for (int i = 0; i < 50; ++i) tick();
    require(course.driving() && car().x > 8.0f && car().x < 16.0f &&
                course.absolute_cell().first == -2147483647LL,
            "crossing the seam tracks the vehicle in the rebased world");
    keys.interact = true;
    tick();
    require(!course.driving() && player().z < 0.001f && player().y > car().y + 1.0f,
            "exit in the streamed cell finds supported collision and clears held brake");
    const auto parked = car();
    keys.interact = false;
    keys.brake = false;
    keys.forward = true;
    for (int i = 0; i < 450; ++i) tick();
    require(player().x > 24.0f && course.resident_count() == 4 && car() == parked,
            "the parked vehicle cell remains resident beyond the pedestrian streaming radius");
    keys.reset = true;
    tick();
    require(player() == saige::Vec3{4, 4, 0} && car() == saige::Vec3{4, 1, 0} &&
                course.absolute_cell().first == -2147483648LL,
            "the original world checkpoint remains exact after walking and driving");
}

void test_rebased_ramp_course(const fs::path& fixture) {
    using namespace std::chrono_literals;
    const auto play = [&] {
        saige::World world;
        CourseKeys keys;
        saige::sample::StreamedCourse course(world, keys, fixture.parent_path() / "ramp-course-world.json");
        const auto player = [&] { return world.snapshot().front().position; };
        require(player() == saige::Vec3{3, 4, 0}, "version 2 uses the authored checkpoint");
        keys.forward = true;
        std::vector<saige::Vec3> poses;
        for (int frame = 0; frame < 170; ++frame) {
            course.fixed_update(world, 16'666'667ns);
            poses.push_back(player());
        }
        require(course.resident_count() == 2 && player().x > 11 && player().z > 0.7f,
                "walk across a rebased chunk seam and climb the authored triangle ramp");
        keys.reset = true;
        course.fixed_update(world, 16'666'667ns);
        require(player() == saige::Vec3{3, 4, 0}, "reset restores the supported authored point");
        return poses;
    };
    const auto first = play();
    require(first == play(), "rebased ramp traversal replays deterministically");
}

void test_manifest(const std::filesystem::path& fixture) {
    Workspace workspace;
    const auto scene_dir = workspace.root / "scenes";
    fs::copy(fixture.parent_path(), scene_dir, fs::copy_options::recursive);
    const auto manifest_path = workspace.root / "world.json";
    write_chunk_fixture(manifest_path, R"({"schema_version":1,"chunks":[
        {"coord":[1,0],"project":"scenes/saige.scene.json"},
        {"coord":[-1,0],"project":"scenes/missing.scene.json"},
        {"coord":[0,0],"project":"scenes/saige.scene.json"}
    ]})");
    const auto manifest = saige::load_chunk_manifest(manifest_path);
    require(manifest.size() == 3 && manifest[0].coord == ChunkCoord{-1, 0} &&
            manifest[1].coord == ChunkCoord{0, 0} && manifest[2].coord == ChunkCoord{1, 0}, "sort manifest coordinates");
    const auto mesh = saige::PreparedScene(fixture).assets().front().id;
    ChunkStream stream;
    require(saige::chunk_containing({-0.01f, -8.0f, 0}, 8) == ChunkCoord{-1, -1}, "negative cells use floor");
    for (const float x : {0.0f, 8.0f, 7.99f}) {
        const auto view = saige::chunk_containing({x, 0, 0}, 8);
        stream.request(saige::select_chunks(manifest, view, 0));
        require(stream.stage_next() && stream.commit_frame(), "cross or revisit a manifest chunk");
        require(stream.active_count() == 1 && stream.active_chunk(view)->find_asset(mesh), "revisit preserves the saved mesh ID");
    }
    const auto* west = stream.active_chunk({0, 0});
    stream.request(saige::select_chunks(manifest, saige::chunk_containing({-0.1f, 0, 0}, 8), 0));
    bool missing = false;
    try {
        (void)stream.stage_next();
    } catch (const std::exception&) {
        missing = true;
    }
    require(missing && !stream.commit_frame() && stream.active_chunk({0, 0}) == west, "missing project retains current residency");
    require(saige::select_chunks(manifest, {0, 0}, 1).size() == 3, "radius includes both neighboring cells");
    require(saige::select_chunks(manifest, {std::numeric_limits<std::int32_t>::min(), 0}, 1).empty(),
            "extreme coordinates do not overflow");

    try {
        (void)saige::chunk_containing({1, 0, 0}, 1e-30);
        throw std::runtime_error("out-of-range grid coordinate was accepted");
    } catch (const std::out_of_range&) {
    }

    for (const auto malformed : {
        R"({"schema_version":2,"chunks":[]})",
        R"({"schema_version":1,"chunks":[[[[[[[[[[]]]]]]]]]]})",
        R"({"schema_version":1,"chunks":[{"coord":[0,1.0],"project":"a"}]})",
        R"({"schema_version":1,"chunks":[{"coord":[4294967295,0],"project":"a"}]})",
        R"({"schema_version":1,"chunks":[{"coord":[0,0],"project":"../a"}]})",
        R"({"schema_version":1,"chunks":[{"coord":[0,0],"project":"scene.json","collision":"../a"}]})",
        R"({"schema_version":1,"chunks":[{"coord":[0,0],"project":"a"},{"coord":[0,0],"project":"b"}]})"
    }) {
        write_chunk_fixture(manifest_path, malformed);
        bool rejected = false;
        try {
            (void)saige::load_chunk_manifest(manifest_path);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid manifest was accepted");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        test_stream(argv[1]);
        test_manifest(argv[1]);
        test_native_streamed_course(argv[1]);
        test_rebased_ramp_course(argv[1]);
        test_streamed_vehicle(argv[1]);
        test_live_origin_migration(argv[1]);
        test_persistent_checkpoint(argv[1]);
        test_collision_handoff(argv[1]);
        test_background_stage(argv[1]);
        test_shared_project_preparation(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
