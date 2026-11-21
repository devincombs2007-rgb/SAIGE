#include "saige/gfx_test/saige_test.h"

#include "saige/rhi/resource_desc.h"

#include <limits>

using namespace saige::rhi;

static_assert(maxMipLevels(1) == 1);
static_assert(maxMipLevels(2) == 2);
static_assert(maxMipLevels(1024, 512) == 11);
static_assert(maxMipLevels(1920, 1080) == 11);  // 1920 -> ... -> 1 is 11 levels
static_assert(maxMipLevels(4, 4, 64) == 7);
static_assert(mipExtent({1920, 1080, 1}, 10) == Extent3D{1, 1, 1});
static_assert(mipExtent({1920, 1080, 1}, 3) == Extent3D{240, 135, 1});

namespace {

TextureDesc makeTexture2D(Format format, uint32_t w, uint32_t h, TextureUsage usage)
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

    d.structureStride = 16;
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
    d.structureStride = 6;
    CHECK_FALSE(validate(d).ok());  // stride not multiple of 4

    d.structureStride = 24;
    CHECK_FALSE(validate(d).ok());  // size not multiple of stride

    d.structureStride = 0;
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
    CHECK(validate(makeTexture2D(Format::RGBA16_FLOAT, 1920, 1080,
                                 TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::Storage))
              .ok());
    CHECK(validate(makeTexture2D(Format::D32_FLOAT, 1920, 1080,
                                 TextureUsage::DepthStencilAttachment | TextureUsage::Sampled))
              .ok());

    TextureDesc albedo = makeTexture2D(Format::BC7_SRGB, 2048, 2048, TextureUsage::Sampled | TextureUsage::TransferDst);
    albedo.mipLevels = kFullMipChain;
    CHECK(validate(albedo).ok());
    CHECK(resolveMipLevels(albedo) == 12);

    TextureDesc cube = makeTexture2D(Format::RGBA16_FLOAT, 256, 256, TextureUsage::Sampled);
    cube.dimension = TextureDimension::TextureCube;
    cube.arrayLayers = 6;
    cube.mipLevels = kFullMipChain;
    CHECK(validate(cube).ok());

    TextureDesc volume = makeTexture2D(Format::RGBA16_FLOAT, 160, 90, TextureUsage::Storage | TextureUsage::Sampled);
    volume.dimension = TextureDimension::Texture3D;  // e.g., froxel volumetric fog
    volume.depth = 64;
    CHECK(validate(volume).ok());

    TextureDesc msaa = makeTexture2D(Format::RGBA8_UNORM, 1280, 720, TextureUsage::ColorAttachment);
    msaa.sampleCount = 4;
    CHECK(validate(msaa).ok());
}

TEST_CASE("TextureDesc: dimension and extent rules")
{
    TextureDesc d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.format = Format::Unknown;
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 0, 64, TextureUsage::Sampled);
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::None);
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 4, TextureUsage::Sampled);
    d.dimension = TextureDimension::Texture1D;
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.depth = 2;
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 32, TextureUsage::Sampled);
    d.dimension = TextureDimension::TextureCube;
    d.arrayLayers = 6;
    CHECK_FALSE(validate(d).ok());  // non-square cube
    d.height = 64;
    d.arrayLayers = 5;
    CHECK_FALSE(validate(d).ok());  // not a multiple of 6

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.dimension = TextureDimension::Texture3D;
    d.arrayLayers = 2;
    CHECK_FALSE(validate(d).ok());  // 3D arrays
}

TEST_CASE("TextureDesc: mip and sample count rules")
{
    TextureDesc d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::Sampled);
    d.mipLevels = 7;
    CHECK(validate(d).ok());
    d.mipLevels = 8;
    CHECK_FALSE(validate(d).ok());

    d.mipLevels = 1;
    d.sampleCount = 3;
    CHECK_FALSE(validate(d).ok());

    d = makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::ColorAttachment);
    d.sampleCount = 4;
    d.mipLevels = 2;
    CHECK_FALSE(validate(d).ok());  // MSAA with mips

    d.mipLevels = 1;
    d.usage |= TextureUsage::Storage;
    CHECK_FALSE(validate(d).ok());  // MSAA storage
}

TEST_CASE("TextureDesc: format/usage compatibility")
{
    CHECK_FALSE(validate(makeTexture2D(Format::BC7_UNORM, 64, 64, TextureUsage::ColorAttachment)).ok());
    CHECK_FALSE(validate(makeTexture2D(Format::BC5_UNORM, 64, 64, TextureUsage::Storage)).ok());
    CHECK_FALSE(validate(makeTexture2D(Format::D32_FLOAT, 64, 64, TextureUsage::ColorAttachment)).ok());
    CHECK_FALSE(validate(makeTexture2D(Format::D32_FLOAT, 64, 64, TextureUsage::Storage)).ok());
    CHECK_FALSE(validate(makeTexture2D(Format::RGBA8_UNORM, 64, 64, TextureUsage::DepthStencilAttachment)).ok());
    CHECK_FALSE(validate(makeTexture2D(Format::RGBA8_SRGB, 64, 64, TextureUsage::Storage)).ok());

    TextureDesc depth3D = makeTexture2D(Format::D16_UNORM, 64, 64, TextureUsage::Sampled);
    depth3D.dimension = TextureDimension::Texture3D;
    CHECK_FALSE(validate(depth3D).ok());
}

TEST_CASE("TextureDesc: validation errors carry a message")
{
    const ValidationResult r = validate(makeTexture2D(Format::Unknown, 1, 1, TextureUsage::Sampled));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error != nullptr);
}

TEST_CASE("computeTextureDataSize: matches hand-computed sizes")
{
    // 4x4 RGBA8, full chain: 4x4 + 2x2 + 1x1 = 21 texels * 4 bytes
    TextureDesc d = makeTexture2D(Format::RGBA8_UNORM, 4, 4, TextureUsage::Sampled);
    d.mipLevels = kFullMipChain;
    CHECK(computeTextureDataSize(d) == 84);

    // Cube: 6 faces
    d.dimension = TextureDimension::TextureCube;
    d.arrayLayers = 6;
    CHECK(computeTextureDataSize(d) == 84u * 6u);

    // BC1 8x8 full chain: 2x2 blocks + 1 block + 1 block + 1 block = 7 blocks * 8 bytes
    TextureDesc bc = makeTexture2D(Format::BC1_UNORM, 8, 8, TextureUsage::Sampled);
    bc.mipLevels = kFullMipChain;
    CHECK(computeTextureDataSize(bc) == 56);

    // 3D 4x4x4 R8, 3 mips: 64 + 8 + 1
    TextureDesc vol = makeTexture2D(Format::R8_UNORM, 4, 4, TextureUsage::Sampled);
    vol.dimension = TextureDimension::Texture3D;
    vol.depth = 4;
    vol.mipLevels = kFullMipChain;
    CHECK(computeTextureDataSize(vol) == 73);
}

// ----------------------------------------------------------------------------------------------------- samplers

TEST_CASE("SamplerDesc: validation")
{
    SamplerDesc s;
    CHECK(validate(s).ok());
    s.maxAnisotropy = 16.0f;
    CHECK(validate(s).ok());
    s.maxAnisotropy = 0.5f;
    CHECK_FALSE(validate(s).ok());
    s.maxAnisotropy = 1.0f;
    s.minLod = 4.0f;
    s.maxLod = 2.0f;
    CHECK_FALSE(validate(s).ok());
    s.maxLod = kLodClampNone;
    s.mipLodBias = std::numeric_limits<float>::infinity();
    CHECK_FALSE(validate(s).ok());
}

TEST_CASE("SamplerDesc: equal descriptions hash equally, different ones differ")
{
    SamplerDesc a;
    SamplerDesc b;
    CHECK(a == b);
    CHECK(hashSamplerDesc(a) == hashSamplerDesc(b));

    b.mipLodBias = -0.0f;  // canonicalized
    CHECK(hashSamplerDesc(a) == hashSamplerDesc(b));

    b.addressU = AddressMode::ClampToEdge;
    CHECK(a != b);
    CHECK(hashSamplerDesc(a) != hashSamplerDesc(b));

    SamplerDesc shadow;
    shadow.compareEnable = true;
    shadow.compareOp = CompareOp::Greater;  // reverse-Z
    CHECK(hashSamplerDesc(shadow) != hashSamplerDesc(a));
}
