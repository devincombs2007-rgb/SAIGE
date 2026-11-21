#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/resource_desc.hpp"

#include <limits>

using namespace saige::rhi;

static_assert(max_mip_levels(1) == 1);
static_assert(max_mip_levels(2) == 2);
static_assert(max_mip_levels(1024, 512) == 11);
static_assert(max_mip_levels(1920, 1080) == 11);  // 1920 -> ... -> 1 is 11 levels
static_assert(max_mip_levels(4, 4, 64) == 7);
static_assert(mip_extent({1920, 1080, 1}, 10) == Extent3D{1, 1, 1});
static_assert(mip_extent({1920, 1080, 1}, 3) == Extent3D{240, 135, 1});

namespace {

TextureDesc make_texture_2d(Format format, uint32_t w, uint32_t h, TextureUsage usage)
{
    TextureDesc d;
    d.format = format;
    d.width = w;
    d.height = h;
    d.usage = usage;
    return d;
}

} // namespace

// ----------------------------------------------------------------------------------------------------- buffers

TEST_CASE("BufferDesc: valid descriptions pass")
{
    BufferDesc d;
    d.size = 1024;
    d.usage = BufferUsage::Storage | BufferUsage::TransferDst;
    CHECK(validate(d).ok());

    d.structure_stride = 16;
    CHECK(validate(d).ok());
}

TEST_CASE("BufferDesc: invalid descriptions are rejected")
{
    BufferDesc d;
    d.usage = BufferUsage::Vertex;
    CHECK_FALSE(validate(d).ok());  // zero size

    d.size = 64;
    d.usage = BufferUsage::None;
    CHECK_FALSE(validate(d).ok());

    d.usage = BufferUsage::Storage;
    d.structure_stride = 6;
    CHECK_FALSE(validate(d).ok());  // stride not multiple of 4

    d.structure_stride = 24;
    CHECK_FALSE(validate(d).ok());  // size not multiple of stride

    d.structure_stride = 0;
    d.usage = BufferUsage::AccelerationStructureStorage;
    CHECK_FALSE(validate(d).ok());  // RT usage without DeviceAddress
    d.usage |= BufferUsage::DeviceAddress;
    CHECK(validate(d).ok());

    d.usage = BufferUsage::Vertex;
    d.memory = MemoryLocation::GpuToCpu;
    CHECK_FALSE(validate(d).ok());
}

// ----------------------------------------------------------------------------------------------------- textures

TEST_CASE("TextureDesc: common render targets and assets are valid")
{
    CHECK(validate(make_texture_2d(Format::RGBA16_FLOAT, 1920, 1080,
                                 TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::Storage))
              .ok());
    CHECK(validate(make_texture_2d(Format::D32_FLOAT, 1920, 1080,
                                 TextureUsage::DepthStencilAttachment | TextureUsage::Sampled))
              .ok());

    TextureDesc albedo = make_texture_2d(Format::BC7_SRGB, 2048, 2048, TextureUsage::Sampled | TextureUsage::TransferDst);
    albedo.mip_levels = full_mip_chain;
    CHECK(validate(albedo).ok());
    CHECK(resolve_mip_levels(albedo) == 12);

    TextureDesc cube = make_texture_2d(Format::RGBA16_FLOAT, 256, 256, TextureUsage::Sampled);
    cube.dimension = TextureDimension::TextureCube;
    cube.array_layers = 6;
    cube.mip_levels = full_mip_chain;
    CHECK(validate(cube).ok());

    TextureDesc volume = make_texture_2d(Format::RGBA16_FLOAT, 160, 90, TextureUsage::Storage | TextureUsage::Sampled);
    volume.dimension = TextureDimension::Texture3D;  // e.g., froxel volumetric fog
    volume.depth = 64;
    CHECK(validate(volume).ok());

    TextureDesc msaa = make_texture_2d(Format::RGBA8_UNORM, 1280, 720, TextureUsage::ColorAttachment);
    msaa.sample_count = 4;
    CHECK(validate(msaa).ok());
}

TEST_CASE("TextureDesc: dimension and extent rules")
{
    TextureDesc d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.format = Format::Unknown;
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 0, 64, TextureUsage::Sampled);
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::None);
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 4, TextureUsage::Sampled);
    d.dimension = TextureDimension::Texture1D;
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.depth = 2;
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 32, TextureUsage::Sampled);
    d.dimension = TextureDimension::TextureCube;
    d.array_layers = 6;
    CHECK_FALSE(validate(d).ok());  // non-square cube
    d.height = 64;
    d.array_layers = 5;
    CHECK_FALSE(validate(d).ok());  // not a multiple of 6

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.dimension = TextureDimension::Texture3D;
    d.array_layers = 2;
    CHECK_FALSE(validate(d).ok());  // 3D arrays
}

TEST_CASE("TextureDesc: mip and sample count rules")
{
    TextureDesc d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.mip_levels = 7;
    CHECK(validate(d).ok());
    d.mip_levels = 8;
    CHECK_FALSE(validate(d).ok());

    d.mip_levels = 1;
    d.sample_count = 3;
    CHECK_FALSE(validate(d).ok());

    d = make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::ColorAttachment);
    d.sample_count = 4;
    d.mip_levels = 2;
    CHECK_FALSE(validate(d).ok());  // MSAA with mips

    d.mip_levels = 1;
    d.usage |= TextureUsage::Storage;
    CHECK_FALSE(validate(d).ok());  // MSAA storage
}

TEST_CASE("TextureDesc: format/usage compatibility")
{
    CHECK_FALSE(validate(make_texture_2d(Format::BC7_UNORM, 64, 64, TextureUsage::ColorAttachment)).ok());
    CHECK_FALSE(validate(make_texture_2d(Format::BC5_UNORM, 64, 64, TextureUsage::Storage)).ok());
    CHECK_FALSE(validate(make_texture_2d(Format::D32_FLOAT, 64, 64, TextureUsage::ColorAttachment)).ok());
    CHECK_FALSE(validate(make_texture_2d(Format::D32_FLOAT, 64, 64, TextureUsage::Storage)).ok());
    CHECK_FALSE(validate(make_texture_2d(Format::RGBA8_UNORM, 64, 64, TextureUsage::DepthStencilAttachment)).ok());
    CHECK_FALSE(validate(make_texture_2d(Format::RGBA8_SRGB, 64, 64, TextureUsage::Storage)).ok());

    TextureDesc depth_3d = make_texture_2d(Format::D16_UNORM, 64, 64, TextureUsage::Sampled);
    depth_3d.dimension = TextureDimension::Texture3D;
    CHECK_FALSE(validate(depth_3d).ok());
}

TEST_CASE("TextureDesc: validation errors carry a message")
{
    const ValidationResult r = validate(make_texture_2d(Format::Unknown, 1, 1, TextureUsage::Sampled));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error != nullptr);
}

TEST_CASE("compute_texture_data_size: matches hand-computed sizes")
{
    // 4x4 RGBA8, full chain: 4x4 + 2x2 + 1x1 = 21 texels * 4 bytes
    TextureDesc d = make_texture_2d(Format::RGBA8_UNORM, 4, 4, TextureUsage::Sampled);
    d.mip_levels = full_mip_chain;
    CHECK(compute_texture_data_size(d) == 84);

    // Cube: 6 faces
    d.dimension = TextureDimension::TextureCube;
    d.array_layers = 6;
    CHECK(compute_texture_data_size(d) == 84u * 6u);

    // BC1 8x8 full chain: 2x2 blocks + 1 block + 1 block + 1 block = 7 blocks * 8 bytes
    TextureDesc bc = make_texture_2d(Format::BC1_UNORM, 8, 8, TextureUsage::Sampled);
    bc.mip_levels = full_mip_chain;
    CHECK(compute_texture_data_size(bc) == 56);

    // 3D 4x4x4 R8, 3 mips: 64 + 8 + 1
    TextureDesc vol = make_texture_2d(Format::R8_UNORM, 4, 4, TextureUsage::Sampled);
    vol.dimension = TextureDimension::Texture3D;
    vol.depth = 4;
    vol.mip_levels = full_mip_chain;
    CHECK(compute_texture_data_size(vol) == 73);
}

// ----------------------------------------------------------------------------------------------------- samplers

TEST_CASE("SamplerDesc: validation")
{
    SamplerDesc s;
    CHECK(validate(s).ok());
    s.max_anisotropy = 16.0f;
    CHECK(validate(s).ok());
    s.max_anisotropy = 0.5f;
    CHECK_FALSE(validate(s).ok());
    s.max_anisotropy = 1.0f;
    s.min_lod = 4.0f;
    s.max_lod = 2.0f;
    CHECK_FALSE(validate(s).ok());
    s.max_lod = lod_clamp_none;
    s.mip_lod_bias = std::numeric_limits<float>::infinity();
    CHECK_FALSE(validate(s).ok());
}

TEST_CASE("SamplerDesc: equal descriptions hash equally, different ones differ")
{
    SamplerDesc a;
    SamplerDesc b;
    CHECK(a == b);
    CHECK(hash_sampler_desc(a) == hash_sampler_desc(b));

    b.mip_lod_bias = -0.0f;  // canonicalized
    CHECK(hash_sampler_desc(a) == hash_sampler_desc(b));

    b.address_u = AddressMode::ClampToEdge;
    CHECK(a != b);
    CHECK(hash_sampler_desc(a) != hash_sampler_desc(b));

    SamplerDesc shadow;
    shadow.compare_enable = true;
    shadow.compare_op = CompareOp::Greater;  // reverse-Z
    CHECK(hash_sampler_desc(shadow) != hash_sampler_desc(a));
}
