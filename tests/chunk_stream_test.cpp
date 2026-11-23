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
        test_collision_handoff(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
