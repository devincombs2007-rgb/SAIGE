#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/format.hpp"

using namespace saige::rhi;

// Compile-time checks: the table is constexpr, so key facts are verified at build time.
static_assert(get_format_info(Format::RGBA8_UNORM).bytes_per_block == 4);
static_assert(get_format_info(Format::BC7_SRGB).is_srgb());
static_assert(get_format_info(Format::D32_FLOAT).has_depth && !get_format_info(Format::D32_FLOAT).has_stencil);
static_assert(compute_surface_layout(Format::BC1_UNORM, 1, 1).slice_pitch == 8);

TEST_CASE("Format: every enum value has consistent metadata")
{
    for (std::size_t i = 1; i < format_count; ++i) {
        const auto f = static_cast<Format>(i);
        const FormatInfo& info = get_format_info(f);
        CHECK(info.format == f);
        CHECK(!info.name.empty());
        CHECK(info.bytes_per_block > 0);
        CHECK(info.component_count >= 1);
        CHECK(info.component_count <= 4);
        if (info.compressed) {
            CHECK(info.block_width == 4);
            CHECK(info.block_height == 4);
            CHECK((info.bytes_per_block == 8 || info.bytes_per_block == 16));
        } else {
            CHECK(info.block_width == 1);
            CHECK(info.block_height == 1);
        }
        CHECK(info.is_depth_or_stencil() == (info.type == FormatType::DepthStencil));
    }
}

TEST_CASE("Format: out-of-range values map to Unknown")
{
    CHECK(get_format_info(Format::Count).format == Format::Unknown);
    CHECK(get_format_info(static_cast<Format>(250)).format == Format::Unknown);
}

TEST_CASE("Format: s_rgb <-> linear conversions are inverse pairs")
{
    for (std::size_t i = 1; i < format_count; ++i) {
        const auto f = static_cast<Format>(i);
        if (is_srgb(f)) {
            const Format lin = to_linear(f);
            CHECK(lin != f);
            CHECK_FALSE(is_srgb(lin));
            CHECK(to_srgb(lin) == f);
            CHECK(get_format_info(lin).bytes_per_block == get_format_info(f).bytes_per_block);
        } else {
            CHECK(to_linear(f) == f);
        }
    }
    CHECK(to_srgb(Format::RGBA16_FLOAT) == Format::RGBA16_FLOAT);  // no s_rgb variant
}

TEST_CASE("Format: depth/stencil classification")
{
    CHECK(has_depth(Format::D24_UNORM_S8_UINT));
    CHECK(has_stencil(Format::D24_UNORM_S8_UINT));
    CHECK(has_depth(Format::D16_UNORM));
    CHECK_FALSE(has_stencil(Format::D16_UNORM));
    CHECK_FALSE(is_depth_or_stencil(Format::R32_FLOAT));
}

TEST_CASE("SurfaceLayout: uncompressed formats are tightly packed")
{
    const SurfaceLayout s = compute_surface_layout(Format::RGBA16_FLOAT, 1920, 1080);
    CHECK(s.blocks_wide == 1920);
    CHECK(s.blocks_high == 1080);
    CHECK(s.row_pitch == 1920u * 8u);
    CHECK(s.slice_pitch == 1920ull * 1080ull * 8ull);
}

TEST_CASE("SurfaceLayout: block-compressed formats round up partial blocks")
{
    const SurfaceLayout s = compute_surface_layout(Format::BC7_UNORM, 13, 6);  // 4 x 2 blocks
    CHECK(s.blocks_wide == 4);
    CHECK(s.blocks_high == 2);
    CHECK(s.row_pitch == 64);
    CHECK(s.slice_pitch == 128);

    const SurfaceLayout tiny = compute_surface_layout(Format::BC4_UNORM, 2, 2);
    CHECK(tiny.slice_pitch == 8);
}

TEST_CASE("SurfaceLayout: degenerate inputs produce an empty layout")
{
    CHECK(compute_surface_layout(Format::Unknown, 16, 16).slice_pitch == 0);
    CHECK(compute_surface_layout(Format::RGBA8_UNORM, 0, 16).slice_pitch == 0);
}
