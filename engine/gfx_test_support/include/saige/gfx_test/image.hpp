// Image comparison and golden-image checks for visual regression tests (header-only, no dependencies).
//
// Images are RGBA8 in memory and binary PPM (P6) on disk: trivially portable, byte-exact in git. Comparison is tolerant
// on purpose: a software rasterizer (Mesa lavapipe) can differ by a few counts per channel across Mesa versions or
// CPUs, so a golden check fails only on a meaningful difference (RMSE, or too many pixels beyond a per-channel delta).
//
// Environment:
//   SAIGE_UPDATE_GOLDENS=1   rewrite goldens instead of comparing (review the resulting git diff!)
//   SAIGE_CAPTURE_DIR=<dir>  where failure artifacts (actual / expected / amplified diff) are written; defaults to
//                            "<golden dir>/_failures" (git-ignored)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace saige::test {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  ///< width * height * 4

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0 && rgba.size() == size_t{width} * height * 4; }
    [[nodiscard]] const uint8_t* at(uint32_t x, uint32_t y) const { return &rgba[(size_t{y} * width + x) * 4]; }
    [[nodiscard]] uint8_t* at(uint32_t x, uint32_t y) { return &rgba[(size_t{y} * width + x) * 4]; }
};

[[nodiscard]] inline Image make_image(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(size_t{w} * h * 4);
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

/// Writes a binary PPM (alpha is dropped). Returns false on I/O failure.
inline bool write_ppm(const std::filesystem::path& path, const Image& img)
{
    if (!img.valid()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    for (size_t i = 0; i < size_t{img.width} * img.height; ++i) {
        out.write(reinterpret_cast<const char*>(&img.rgba[i * 4]), 3);
    }
    return static_cast<bool>(out);
}

/// Reads a binary PPM (P6, 8 bit). Returns nullopt for a missing file or any malformed/truncated content.
[[nodiscard]] inline std::optional<Image> read_ppm(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string magic;
    in >> magic;
    if (magic != "P6") {
        return std::nullopt;
    }
    auto read_number = [&in]() -> std::optional<uint32_t> {
        while (in && (in.peek() == '#' || std::isspace(in.peek()) != 0)) {  // comments and whitespace
            if (in.peek() == '#') {
                std::string skip;
                std::getline(in, skip);
            } else {
                in.get();
            }
        }
        uint32_t value = 0;
        if (!(in >> value)) {
            return std::nullopt;
        }
        return value;
    };
    const auto w = read_number();
    const auto h = read_number();
    const auto maxval = read_number();
    if (!w || !h || !maxval || *maxval != 255 || *w == 0 || *h == 0 || *w > 16384 || *h > 16384) {
        return std::nullopt;
    }
    in.get();  // the single whitespace after maxval
    Image img;
    img.width = *w;
    img.height = *h;
    img.rgba.assign(size_t{*w} * *h * 4, 255);
    std::vector<uint8_t> row(size_t{*w} * 3);
    for (uint32_t y = 0; y < *h; ++y) {
        in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
        if (in.gcount() != static_cast<std::streamsize>(row.size())) {
            return std::nullopt;  // truncated
        }
        for (uint32_t x = 0; x < *w; ++x) {
            uint8_t* px = img.at(x, y);
            px[0] = row[size_t{x} * 3 + 0];
            px[1] = row[size_t{x} * 3 + 1];
            px[2] = row[size_t{x} * 3 + 2];
        }
    }
    return img;
}

struct CompareOptions {
    double max_rmse = 2.0;                       ///< root-mean-square error over RGB, in 0..255 counts
    int channel_tolerance = 16;                  ///< a pixel "differs" if any channel is off by more than this
    double max_fraction_over_tolerance = 0.005;  ///< allowed share of differing pixels
};

struct CompareResult {
    bool same_size = false;
    double rmse = 0.0;
    int max_channel_delta = 0;
    uint64_t pixels_over_tolerance = 0;
    double fraction_over_tolerance = 0.0;
    bool passed = false;
};

[[nodiscard]] inline CompareResult compare_images(const Image& actual, const Image& expected, const CompareOptions& opt = {})
{
    CompareResult r;
    r.same_size = actual.valid() && expected.valid() && actual.width == expected.width && actual.height == expected.height;
    if (!r.same_size) {
        return r;
    }
    const size_t pixels = size_t{actual.width} * actual.height;
    double sum_sq = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        int pixel_max = 0;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int{actual.rgba[i * 4 + static_cast<size_t>(c)]} -
                                   int{expected.rgba[i * 4 + static_cast<size_t>(c)]});
            sum_sq += static_cast<double>(d) * static_cast<double>(d);
            pixel_max = std::max(pixel_max, d);
        }
        r.max_channel_delta = std::max(r.max_channel_delta, pixel_max);
        r.pixels_over_tolerance += pixel_max > opt.channel_tolerance ? 1u : 0u;
    }
    r.rmse = std::sqrt(sum_sq / static_cast<double>(pixels * 3));
    r.fraction_over_tolerance = static_cast<double>(r.pixels_over_tolerance) / static_cast<double>(pixels);
    r.passed = r.rmse <= opt.max_rmse && r.fraction_over_tolerance <= opt.max_fraction_over_tolerance;
    return r;
}

/// Absolute per-channel difference amplified by `gain` (clamped), for human inspection. Sizes must match.
[[nodiscard]] inline Image make_diff_image(const Image& actual, const Image& expected, int gain = 8)
{
    Image d;
    if (!actual.valid() || !expected.valid() || actual.width != expected.width || actual.height != expected.height) {
        return d;
    }
    d.width = actual.width;
    d.height = actual.height;
    d.rgba.resize(actual.rgba.size());
    for (size_t i = 0; i < size_t{d.width} * d.height; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const int delta = std::abs(int{actual.rgba[i * 4 + c]} - int{expected.rgba[i * 4 + c]});
            d.rgba[i * 4 + c] = static_cast<uint8_t>(std::min(255, delta * gain));
        }
        d.rgba[i * 4 + 3] = 255;
    }
    return d;
}

struct GoldenOutcome {
    enum class Status { Matched, Updated, Missing, Mismatch, SizeMismatch, Unreadable };
    Status status = Status::Missing;
    CompareResult comparison;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return status == Status::Matched || status == Status::Updated; }
};

[[nodiscard]] inline bool update_goldens_requested()
{
    const char* v = std::getenv("SAIGE_UPDATE_GOLDENS");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

/// Compares `actual` with `<golden_dir>/<name>.ppm`. On mismatch, writes `<name>.actual.ppm`, `<name>.expected.ppm`
/// and `<name>.diff.ppm` into the failure directory so CI can upload them.
[[nodiscard]] inline GoldenOutcome check_golden(const std::filesystem::path& golden_dir, const std::string& name,
                                                const Image& actual, const CompareOptions& opt = {})
{
    GoldenOutcome out;
    const std::filesystem::path golden = golden_dir / (name + ".ppm");
    if (!actual.valid()) {
        out.status = GoldenOutcome::Status::Unreadable;
        out.message = "actual image is empty or malformed";
        return out;
    }
    if (update_goldens_requested()) {
        if (!write_ppm(golden, actual)) {
            out.status = GoldenOutcome::Status::Unreadable;
            out.message = "could not write " + golden.string();
            return out;
        }
        out.status = GoldenOutcome::Status::Updated;
        out.message = "updated " + golden.string();
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::exists(golden, ec)) {
        out.status = GoldenOutcome::Status::Missing;
        out.message = "no golden at " + golden.string() + " (create it with SAIGE_UPDATE_GOLDENS=1 and review the image)";
        return out;
    }
    const std::optional<Image> expected = read_ppm(golden);
    if (!expected) {
        out.status = GoldenOutcome::Status::Unreadable;
        out.message = "golden " + golden.string() + " is not a valid 8-bit P6 PPM";
        return out;
    }
    out.comparison = compare_images(actual, *expected, opt);
    const char* capture = std::getenv("SAIGE_CAPTURE_DIR");
    const std::filesystem::path failures = capture != nullptr && capture[0] != '\0' ? std::filesystem::path(capture)
                                                                                   : golden_dir / "_failures";
    if (!out.comparison.same_size) {
        out.status = GoldenOutcome::Status::SizeMismatch;
        std::ostringstream m;
        m << "size mismatch: actual " << actual.width << "x" << actual.height << ", golden " << expected->width << "x"
          << expected->height;
        out.message = m.str();
        write_ppm(failures / (name + ".actual.ppm"), actual);
        write_ppm(failures / (name + ".expected.ppm"), *expected);
        return out;
    }
    if (out.comparison.passed) {
        out.status = GoldenOutcome::Status::Matched;
        return out;
    }
    out.status = GoldenOutcome::Status::Mismatch;
    std::ostringstream m;
    m << "'" << name << "' differs from its golden: RMSE " << out.comparison.rmse << " (max " << opt.max_rmse << "), "
      << out.comparison.pixels_over_tolerance << " pixels (" << out.comparison.fraction_over_tolerance * 100.0
      << "%) beyond +/-" << opt.channel_tolerance << " (max " << opt.max_fraction_over_tolerance * 100.0
      << "%), largest channel delta " << out.comparison.max_channel_delta << "; artifacts in " << failures.string();
    out.message = m.str();
    write_ppm(failures / (name + ".actual.ppm"), actual);
    write_ppm(failures / (name + ".expected.ppm"), *expected);
    write_ppm(failures / (name + ".diff.ppm"), make_diff_image(actual, *expected));
    return out;
}

} // namespace saige::test
