#include "saige/chunk_stream.hpp"

#include <array>
#include <filesystem>
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
    require(stream.stage_next() && stream.commit_frame() && stream.active_chunk(west.coord)->find_asset(mesh), "retry preserves stable mesh ID");

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

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        test_stream(argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
