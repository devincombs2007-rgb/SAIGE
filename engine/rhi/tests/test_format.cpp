#include "saige/gfx_test/saige_test.h"

#include "saige/rhi/format.h"

using namespace saige::rhi;

// Compile-time checks: the table is constexpr, so key facts are verified at build time.
static_assert(getFormatInfo(Format::RGBA8_UNORM).bytesPerBlock == 4);
static_assert(getFormatInfo(Format::BC7_SRGB).isSrgb());
static_assert(getFormatInfo(Format::D32_FLOAT).hasDepth && !getFormatInfo(Format::D32_FLOAT).hasStencil);
static_assert(computeSurfaceLayout(Format::BC1_UNORM, 1, 1).slicePitch == 8);

TEST_CASE("Format: every enum value has consistent metadata")
{
    for (std::size_t i = 1; i < kFormatCount; ++i) {
        const auto f = static_cast<Format>(i);
        const FormatInfo& info = getFormatInfo(f);
        CHECK(info.format == f);
        CHECK(!info.name.empty());
        CHECK(info.bytesPerBlock > 0);
        CHECK(info.componentCount >= 1);
        CHECK(info.componentCount <= 4);
        if (info.compressed) {
            CHECK(info.blockWidth == 4);
            CHECK(info.blockHeight == 4);
            CHECK((info.bytesPerBlock == 8 || info.bytesPerBlock == 16));
        } else {
            CHECK(info.blockWidth == 1);
            CHECK(info.blockHeight == 1);
        }
        CHECK(info.isDepthOrStencil() == (info.type == FormatType::DepthStencil));
    }
}

TEST_CASE("Format: out-of-range values map to Unknown")
{
    CHECK(getFormatInfo(Format::Count).format == Format::Unknown);
    CHECK(getFormatInfo(static_cast<Format>(250)).format == Format::Unknown);
}

TEST_CASE("Format: sRGB <-> linear conversions are inverse pairs")
{
    for (std::size_t i = 1; i < kFormatCount; ++i) {
        const auto f = static_cast<Format>(i);
        if (isSrgb(f)) {
            const Format lin = toLinear(f);
            CHECK(lin != f);
            CHECK_FALSE(isSrgb(lin));
            CHECK(toSrgb(lin) == f);
            CHECK(getFormatInfo(lin).bytesPerBlock == getFormatInfo(f).bytesPerBlock);
        } else {
            CHECK(toLinear(f) == f);
        }
    }
    CHECK(toSrgb(Format::RGBA16_FLOAT) == Format::RGBA16_FLOAT);  // no sRGB variant
}

TEST_CASE("Format: depth/stencil classification")
{
    CHECK(hasDepth(Format::D24_UNORM_S8_UINT));
    CHECK(hasStencil(Format::D24_UNORM_S8_UINT));
    CHECK(hasDepth(Format::D16_UNORM));
    CHECK_FALSE(hasStencil(Format::D16_UNORM));
    CHECK_FALSE(isDepthOrStencil(Format::R32_FLOAT));
}

TEST_CASE("SurfaceLayout: uncompressed formats are tightly packed")
{
    const SurfaceLayout s = computeSurfaceLayout(Format::RGBA16_FLOAT, 1920, 1080);
    CHECK(s.blocksWide == 1920);
    CHECK(s.blocksHigh == 1080);
    CHECK(s.rowPitch == 1920u * 8u);
    CHECK(s.slicePitch == 1920ull * 1080ull * 8ull);
}

TEST_CASE("SurfaceLayout: block-compressed formats round up partial blocks")
{
    const SurfaceLayout s = computeSurfaceLayout(Format::BC7_UNORM, 13, 6);  // 4 x 2 blocks
    CHECK(s.blocksWide == 4);
    CHECK(s.blocksHigh == 2);
    CHECK(s.rowPitch == 64);
    CHECK(s.slicePitch == 128);

    const SurfaceLayout tiny = computeSurfaceLayout(Format::BC4_UNORM, 2, 2);
    CHECK(tiny.slicePitch == 8);
}

TEST_CASE("SurfaceLayout: degenerate inputs produce an empty layout")
{
    CHECK(computeSurfaceLayout(Format::Unknown, 16, 16).slicePitch == 0);
    CHECK(computeSurfaceLayout(Format::RGBA8_UNORM, 0, 16).slicePitch == 0);
}
