// Large-world precision: a global position is a cell plus a chunk-local float, so its resolution does not depend on the distance from the world origin. Measured against a float world position and a double reference,
// across large distances, cell borders, negative coordinates and origin migration of a driving vehicle.
#include "saige/large_world.hpp"
#include "saige/static_collision.hpp"
#include "saige/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr double cell = 256.0;

template <class F>
bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::uint32_t lcg_state = 12345u;
double rnd() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return static_cast<double>(lcg_state >> 8) / 16777216.0;
}

void test_resolution() {
    std::printf("distance from the origin: resolution of a float world position | of the local part (cell %.0f m)\n", cell);
    double at_100km = 0.0, local_worst = 0.0;
    for (const double d : {1e3, 1e4, 1e5, 1e6, 1e7}) {
        double local_res = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const WorldPosition p = to_world_position({d + 1000.0 * rnd(), -d - 1000.0 * rnd(), 5.0}, cell);
            local_res = std::max(local_res, float_resolution(p.local.x));
        }
        std::printf("  %9.0f m: %.2e m | %.2e m\n", d, float_resolution(d), local_res);
        if (d == 1e5) at_100km = float_resolution(d);
        local_worst = std::max(local_worst, local_res);
    }
    require(at_100km > 7e-3, "control: a float world position at 100 km resolves worse than 7 mm");
    require(local_worst <= 1.6e-5, "the local part resolves 16 micrometers or better at every distance, to 10,000 km");
}

void test_round_trip() {
    double worst = 0.0;
    for (const double mag : {10.0, 1e3, 1e5, 1e7, 1e9}) {
        for (int i = 0; i < 4000; ++i) {
            const DVec3 g{(rnd() * 2 - 1) * mag, (rnd() * 2 - 1) * mag, (rnd() * 2 - 1) * 50.0};
            const WorldPosition p = to_world_position(g, cell);
            require(p.local.x >= 0.0f && p.local.x < cell && p.local.y >= 0.0f && p.local.y < cell, "the local offset is in [0, cell) (x and y)");
            const DVec3 back = to_global(p, cell);
            worst = std::max({worst, std::fabs(back.x - g.x), std::fabs(back.y - g.y)});
            require(std::fabs(back.z - g.z) < 4e-6, "z is a float of its own (meters, not chunked)");
        }
    }
    std::printf("round trip global -> cell + float -> global, up to 1e9 m: worst error %.2e m\n", worst);
    require(worst <= 0.5 * 1.6e-5 + 1e-9, "the round trip error is half a float step of the local part, whatever the distance");
}

void test_borders() {
    // Exactly on a border: the next cell, offset 0. Negative coordinates floor, not truncate.
    WorldPosition p = to_world_position({512.0, -256.0, 0.0}, cell);
    require(p.cell.x == 2 && p.local.x == 0.0f && p.cell.y == -1 && p.local.y == 0.0f, "a coordinate on a border is the next cell at offset 0 (negative ones floor)");
    p = to_world_position({-0.5, -255.5, 0.0}, cell);
    require(p.cell.x == -1 && p.local.x == 255.5f && p.cell.y == -1 && p.local.y == 0.5f, "-0.5 is in cell -1 at 255.5");
    // Just under a border by less than half a float step: rounds up into the next cell instead of an offset equal to the cell size.
    p = to_world_position({3.0 * cell - 1e-9, 0.0, 0.0}, cell);
    require(p.cell.x == 3 && p.local.x == 0.0f, "an offset that rounds to the cell size carries into the next cell");
    // Just under a border at huge distances, where the division itself rounds across it: always a valid offset and a faithful round trip.
    int near_border = 0;
    for (const double mag : {1e6, 1e8, 1e9, 2e9}) {
        for (int i = 0; i < 500; ++i) {
            const double k = std::floor(mag * (0.5 + rnd()) / cell);
            double v = k * cell;
            for (int j = 0; j < 1 + i % 3; ++j) v = std::nextafter(v, -1e300);
            const WorldPosition q = to_world_position({v, -v, 0.0}, cell);
            require(q.local.x >= 0.0f && q.local.x < cell && q.local.y >= 0.0f && q.local.y < cell, "a position within an ulp of a border has an offset in [0, cell)");
            require(std::fabs(to_global(q, cell).x - v) <= 1.6e-5 && std::fabs(to_global(q, cell).y + v) <= 1.6e-5, "and round trips to the local resolution");
            ++near_border;
        }
    }
    require(near_border == 2000, "control: all the border cases ran");
    // A delta that lands within a nanometer under a border carries into the next cell.
    const WorldPosition landed = advance(to_world_position({0.0, 0.0, 0.0}, cell), {255.999999999, 0.0, 0.0}, cell);
    require(landed.cell.x == 1 && landed.local.x == 0.0f, "a delta ending a nanometer under a border lands in the next cell at 0");
    // Two points 1 mm apart on either side of a border far from the origin.
    for (const double base : {1e5, 1e7}) {
        const double border = std::floor(base / cell) * cell;
        const DVec3 a{border - 0.0005, 3.0, 1.0}, b{border + 0.0005, 3.0, 1.0};
        const WorldPosition pa = to_world_position(a, cell), pb = to_world_position(b, cell);
        const DVec3 d = difference(pa, pb, cell);
        const double naive = static_cast<double>(static_cast<float>(b.x)) - static_cast<double>(static_cast<float>(a.x));
        std::printf("  1 mm across a border at %.0f m: the difference is %.6f m, subtracting float world positions gives %.6f m\n", base, d.x, naive);
        require(pa.cell.x != pb.cell.x && std::fabs(d.x - 0.001) < 2e-5, "two points a millimeter apart across a cell border differ by a millimeter (to the local resolution)");
        require(std::fabs(naive - 0.001) > 1e-4, "control: the float world positions cannot tell");
    }
}

void test_relative_and_advance() {
    const WorldPosition p = to_world_position({100000.25, -50000.75, 3.0}, cell);
    const ChunkCoord frame{static_cast<std::int32_t>(std::floor(100000.0 / cell)) - 2, static_cast<std::int32_t>(std::floor(-50000.0 / cell)) + 3};  // a frame a few cells away from the position's own
    const Vec3 r = relative_to(p, frame, cell);
    require(std::fabs(r.x - static_cast<float>(100000.25 - frame.x * cell)) < 1e-5f && std::fabs(r.y - static_cast<float>(-50000.75 - frame.y * cell)) < 1e-5f && r.z == 3.0f, "relative_to gives the frame offset from the cell difference");
    const Vec3 shift = migration_shift({10, -3}, {12, -3}, cell);
    require(shift.x == -512.0f && shift.y == 0.0f && shift.z == 0.0f, "the migration shift is the cell difference times the cell size");
    // Walking 10 km in steps of 0.125 m (exact in float): advance is exact and ends where the double sum does.
    WorldPosition w = to_world_position({0.0, 0.0, 1.0}, cell);
    for (int i = 0; i < 80000; ++i) w = advance(w, {0.125, -0.0625, 0.0}, cell);
    const DVec3 g = to_global(w, cell);
    require(g.x == 10000.0 && g.y == -5000.0, "80,000 exact steps of 0.125 m end exactly at 10 km");
    require(w.local.x >= 0.0f && w.local.x < cell && w.local.y >= 0.0f && w.local.y < cell, "and the offsets are normalized");
    const WorldPosition big = advance(to_world_position({0.0, 0.0, 0.0}, cell), {5e6 + 0.5, -3e6, 0.0}, cell);
    require(to_global(big, cell).x == 5e6 + 0.5, "one big delta lands where it should");
    require(throws([] { (void)to_world_position({std::nan(""), 0, 0}, cell); }) && throws([] { (void)to_world_position({1, 0, 0}, 0.0); }) && throws([] { (void)to_world_position({1, 0, 0}, 1e4); }) && throws([] { (void)to_world_position({1e30, 0, 0}, cell); }) &&
                throws([] { (void)advance(to_world_position({1, 0, 0}, cell), {std::nan(""), 0, 0}, cell); }),
            "non-finite input, a bad cell size and positions off the int32 grid are refused");
}

// A car driven about 170 m down a flat floor (inside the +-256 m envelope), once with no migration and once migrated by 64 m (a power of two: exact in float) each time it passes 40 m: the same path, to the float noise of different contact positions.
void test_vehicle_migration() {
    const StaticCollision floor_world(std::vector<Aabb>{{{-500, -500, -2}, {500, 500, 0}}}, {});
    const auto drive = [&](bool migrate) {
        VehicleConfig vc;
        vc.powertrain.enabled = true;
        Vehicle car(vc, {0.0f, 0.0f, 0.9f}, 0.0f);
        QueryScratch scratch;
        VehicleInput in;
        double offset = 0.0;
        int migrations = 0;
        for (int i = 0; i < 60 * 17; ++i) {
            in.throttle = static_cast<float>(std::clamp((12.0 - car.state().forward_speed) * 0.5, -0.3, 1.0));
            in.steer = 0.02f * std::sin(0.01f * static_cast<float>(i));
            car.step(in, 1.0f / 60.0f, floor_world, scratch);
            if (migrate && car.state().position.x > 40.0f) {
                car.migrate_origin({-64.0f, 0.0f, 0.0f});
                offset += 64.0;
                ++migrations;
            }
        }
        return std::pair<double, int>{offset + static_cast<double>(car.state().position.x), migrations};
    };
    const auto plain_x = [&] {
        // The plain run must stay inside the envelope: floor is large, the car travels about 400 m: the vehicle itself does not limit its range.
        return drive(false);
    }();
    const auto migrated = drive(true);
    std::printf("  a car driven for 17 s: ends at x = %.4f m without migration, %.4f m with %d migrations of 64 m (difference %.2e m)\n", plain_x.first, migrated.first, migrated.second, std::fabs(plain_x.first - migrated.first));
    require(migrated.second >= 3, "control: the car was migrated several times");
    require(plain_x.first > 120.0, "control: it drove a good way");
    require(std::fabs(plain_x.first - migrated.first) < 5e-3, "origin migration changes the vehicle's path by less than 5 mm over 170 m");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"resolution", test_resolution}, {"round_trip", test_round_trip}, {"borders", test_borders}, {"advance", test_relative_and_advance}, {"vehicle", test_vehicle_migration}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "large world tests passed\n";
    return failures == 0 ? 0 : 1;
}
