#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_test/image.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace saige::test;
namespace fs = std::filesystem;

namespace {

fs::path scratch_dir()
{
    const fs::path dir = fs::temp_directory_path() / "saige_gfx_test_image";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void set_env(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

Image gradient(uint32_t w, uint32_t h)
{
    Image img = make_image(w, h, 0, 0, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = img.at(x, y);
            p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            p[2] = 128;
        }
    }
    return img;
}

} // namespace

TEST_CASE("Image: PPM round trip is exact (alpha is dropped and restored opaque)")
{
    const fs::path dir = scratch_dir();
    const Image original = gradient(37, 19);  // odd sizes: no accidental alignment assumptions
    REQUIRE(write_ppm(dir / "a.ppm", original));
    const std::optional<Image> back = read_ppm(dir / "a.ppm");
    REQUIRE(back.has_value());
    CHECK(back->width == 37);
    CHECK(back->height == 19);
    CHECK(back->rgba == original.rgba);
}

TEST_CASE("Image: read_ppm rejects missing, malformed and truncated files")
{
    const fs::path dir = scratch_dir();
    CHECK_FALSE(read_ppm(dir / "missing.ppm").has_value());

    auto write_raw = [&](const char* name, const std::string& content) {
        std::ofstream out(dir / name, std::ios::binary);
        out << content;
        return dir / name;
    };
    CHECK_FALSE(read_ppm(write_raw("p3.ppm", "P3\n1 1\n255\n0 0 0\n")).has_value());        // ASCII variant unsupported
    CHECK_FALSE(read_ppm(write_raw("maxval.ppm", std::string("P6\n1 1\n65535\n") + "abcdef")).has_value());
    CHECK_FALSE(read_ppm(write_raw("zero.ppm", "P6\n0 5\n255\n")).has_value());
    CHECK_FALSE(read_ppm(write_raw("huge.ppm", "P6\n999999 999999\n255\n")).has_value());
    CHECK_FALSE(read_ppm(write_raw("trunc.ppm", std::string("P6\n4 4\n255\n") + "short")).has_value());
    CHECK_FALSE(read_ppm(write_raw("garbage.ppm", "not an image")).has_value());
    // A header comment is legal PPM.
    const auto commented = read_ppm(write_raw("comment.ppm", std::string("P6\n# made by a test\n1 1\n255\n") + "\x01\x02\x03"));
    REQUIRE(commented.has_value());
    CHECK(commented->at(0, 0)[0] == 1);
    CHECK(commented->at(0, 0)[2] == 3);
}

TEST_CASE("Compare: identical images match exactly; metrics are computed correctly")
{
    const Image a = gradient(32, 32);
    const CompareResult same = compare_images(a, a);
    CHECK(same.passed);
    CHECK(same.rmse == 0.0);
    CHECK(same.max_channel_delta == 0);
    CHECK(same.pixels_over_tolerance == 0);

    // One pixel off by 100 in one channel: sum_sq = 10000 over 32*32*3 samples.
    Image b = a;
    b.at(5, 5)[1] = static_cast<uint8_t>(b.at(5, 5)[1] ^ 0x40);  // changes by exactly 64
    const CompareResult one = compare_images(a, b);
    CHECK(one.max_channel_delta == 64);
    CHECK(one.pixels_over_tolerance == 1);
    CHECK(std::fabs(one.rmse - std::sqrt(64.0 * 64.0 / (32.0 * 32.0 * 3.0))) < 1e-9);
}

TEST_CASE("Compare: small noise passes, real differences fail, size mismatch fails")
{
    const Image base = gradient(64, 64);
    // Noise within the per-channel tolerance everywhere: passes (a different Mesa version might do this).
    Image noisy = base;
    for (size_t i = 0; i < noisy.rgba.size(); i += 4) {
        noisy.rgba[i] = static_cast<uint8_t>(noisy.rgba[i] ^ 1);
    }
    CHECK(compare_images(noisy, base).passed);

    // A missing object: a 10x10 block off by 120 => 2.4% of pixels beyond tolerance.
    Image missing = base;
    for (uint32_t y = 0; y < 10; ++y) {
        for (uint32_t x = 0; x < 10; ++x) {
            missing.at(x, y)[0] = static_cast<uint8_t>(missing.at(x, y)[0] ^ 0x78);
        }
    }
    const CompareResult r = compare_images(missing, base);
    CHECK_FALSE(r.passed);
    CHECK(r.fraction_over_tolerance > 0.02);

    // A whole-image brightness shift of 6 counts: no pixel beyond tolerance, but RMSE 6 > 2 => fails.
    Image shifted = base;
    for (size_t i = 0; i < shifted.rgba.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            shifted.rgba[i + c] = static_cast<uint8_t>(std::min(255, shifted.rgba[i + c] + 6));
        }
    }
    CHECK_FALSE(compare_images(shifted, base).passed);

    const CompareResult sized = compare_images(gradient(16, 16), base);
    CHECK_FALSE(sized.same_size);
    CHECK_FALSE(sized.passed);
    CHECK_FALSE(compare_images(Image{}, Image{}).passed);  // empty images are never "equal"
}

TEST_CASE("Diff image: amplifies differences and saturates")
{
    Image a = make_image(2, 1, 10, 10, 10);
    Image b = a;
    b.at(1, 0)[0] = 14;   // delta 4, gain 8 => 32
    b.at(1, 0)[1] = 200;  // delta 190 => saturates
    const Image d = make_diff_image(a, b);
    REQUIRE(d.valid());
    CHECK(d.at(0, 0)[0] == 0);
    CHECK(d.at(1, 0)[0] == 32);
    CHECK(d.at(1, 0)[1] == 255);
    CHECK_FALSE(make_diff_image(a, make_image(3, 3, 0, 0, 0)).valid());
}

TEST_CASE("Golden: missing golden fails with instructions; update then match; mismatch writes artifacts")
{
    const fs::path dir = scratch_dir();
    set_env("SAIGE_UPDATE_GOLDENS", nullptr);
    set_env("SAIGE_CAPTURE_DIR", nullptr);
    const Image img = gradient(48, 27);

    GoldenOutcome missing = check_golden(dir, "scene", img);
    CHECK(missing.status == GoldenOutcome::Status::Missing);
    CHECK(missing.message.find("SAIGE_UPDATE_GOLDENS=1") != std::string::npos);
    CHECK_FALSE(missing.ok());

    set_env("SAIGE_UPDATE_GOLDENS", "1");
    GoldenOutcome updated = check_golden(dir, "scene", img);
    CHECK(updated.status == GoldenOutcome::Status::Updated);
    CHECK(updated.ok());
    CHECK(fs::exists(dir / "scene.ppm"));
    set_env("SAIGE_UPDATE_GOLDENS", nullptr);

    CHECK(check_golden(dir, "scene", img).status == GoldenOutcome::Status::Matched);

    Image broken = img;
    for (uint32_t y = 0; y < 27; ++y) {
        for (uint32_t x = 0; x < 24; ++x) {
            broken.at(x, y)[0] = static_cast<uint8_t>(255 - broken.at(x, y)[0]);  // red spans 0..255, so this is a big change
        }
    }
    GoldenOutcome bad = check_golden(dir, "scene", broken);
    CHECK(bad.status == GoldenOutcome::Status::Mismatch);
    CHECK_FALSE(bad.ok());
    CHECK(fs::exists(dir / "_failures" / "scene.actual.ppm"));
    CHECK(fs::exists(dir / "_failures" / "scene.expected.ppm"));
    CHECK(fs::exists(dir / "_failures" / "scene.diff.ppm"));
    CHECK(bad.message.find("RMSE") != std::string::npos);

    GoldenOutcome wrong_size = check_golden(dir, "scene", gradient(10, 10));
    CHECK(wrong_size.status == GoldenOutcome::Status::SizeMismatch);

    // A corrupt golden is reported, not trusted.
    std::ofstream(dir / "corrupt.ppm", std::ios::binary) << "garbage";
    CHECK(check_golden(dir, "corrupt", img).status == GoldenOutcome::Status::Unreadable);
    CHECK(check_golden(dir, "x", Image{}).status == GoldenOutcome::Status::Unreadable);
}

TEST_CASE("Golden: failure artifacts honor SAIGE_CAPTURE_DIR")
{
    const fs::path dir = scratch_dir();
    const fs::path artifacts = dir / "artifacts";
    set_env("SAIGE_UPDATE_GOLDENS", nullptr);
    REQUIRE(write_ppm(dir / "s.ppm", make_image(8, 8, 0, 0, 0)));
    set_env("SAIGE_CAPTURE_DIR", artifacts.string().c_str());
    const GoldenOutcome bad = check_golden(dir, "s", make_image(8, 8, 255, 255, 255));
    set_env("SAIGE_CAPTURE_DIR", nullptr);
    CHECK(bad.status == GoldenOutcome::Status::Mismatch);
    CHECK(fs::exists(artifacts / "s.diff.ppm"));
    CHECK_FALSE(fs::exists(dir / "_failures"));
}
