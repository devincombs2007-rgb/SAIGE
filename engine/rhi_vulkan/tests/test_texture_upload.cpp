// Texture upload and sampler tests on a real (validated) Vulkan device. CI: lavapipe.
// Round-trips a non-symmetric pattern through upload_texture and reads it back, so a swapped row, channel or
// misplaced mip fails. Validation errors fail each test.

#include "saige/gfx_test/saige_test.hpp"

#include "vulkan_test_util.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

std::byte b(uint32_t v) { return static_cast<std::byte>(v & 0xFFu); }

/// Texel (x, y) of a level/layer: every channel differs and no two texels in a level share a value, so any
/// transposition, flip or channel swap changes the data.
std::vector<std::byte> pattern(uint32_t width, uint32_t height, uint32_t seed)
{
    std::vector<std::byte> bytes;
    bytes.reserve(size_t{width} * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            bytes.push_back(b(x * 16 + seed));
            bytes.push_back(b(y * 29 + 7 + seed));
            bytes.push_back(b(x * 5 + y * 3 + 11));
            bytes.push_back(b(255 - x - y * 2));
        }
    }
    return bytes;
}

rhi::TextureDesc sampled_desc(uint32_t w, uint32_t h, uint32_t mips, uint32_t layers = 1)
{
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = w;
    td.height = h;
    td.mip_levels = mips;
    td.array_layers = layers;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst | rhi::TextureUsage::TransferSrc;
    td.debug_name = "UploadTest";
    return td;
}

/// Reads mip 0, layer 0 back (the RHI's readback path).
std::vector<std::byte> read_back(rhi::Device& device, rhi::TextureHandle texture, uint32_t w, uint32_t h)
{
    rhi::BufferDesc bd;
    bd.size = uint64_t{w} * h * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = "UploadReadback";
    const rhi::BufferHandle readback = device.create_buffer(bd);
    rhi::CommandList& cmd = device.begin_frame();
    const rhi::TextureBarrier to_transfer{
        texture, rhi::sampled_texture_state,
        {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
    cmd.barrier(std::span(&to_transfer, 1));
    cmd.copy_texture_to_buffer(texture, readback);
    (void)device.end_frame();
    device.wait_idle();
    const auto mapped = device.map(readback);
    std::vector<std::byte> out(mapped.begin(), mapped.begin() + static_cast<std::ptrdiff_t>(bd.size));
    device.destroy(readback);
    return out;
}

template <typename Fn>
bool throws_invalid(Fn&& fn)
{
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace

TEST_CASE("Vulkan: uploaded texels read back exactly, with a full mip chain and array layers")
{
    const auto device = make_validated_device("upload_roundtrip");
    // 13 x 7 is deliberately not a power of two (odd mip extents, row pitch not a multiple of 4 texels).
    for (const uint32_t layers : {1u, 3u}) {
        const rhi::TextureDesc td = sampled_desc(13, 7, rhi::full_mip_chain, layers);
        const rhi::TextureHandle tex = device->create_texture(td);
        std::vector<std::byte> data;
        const uint32_t mips = rhi::max_mip_levels(13, 7);
        for (uint32_t level = 0; level < mips; ++level) {
            const rhi::Extent3D e = rhi::mip_extent({13, 7, 1}, level);
            for (uint32_t layer = 0; layer < layers; ++layer) {
                const auto p = pattern(e.width, e.height, level * 40 + layer * 9);
                data.insert(data.end(), p.begin(), p.end());
            }
        }
        REQUIRE(data.size() == rhi::compute_texture_data_size(device->texture_desc(tex).value()));
        device->upload_texture(tex, data);
        const auto got = read_back(*device, tex, 13, 7);
        const auto want = pattern(13, 7, 0);  // level 0, layer 0 is the first block of the data
        REQUIRE(got.size() == want.size());
        CHECK(std::memcmp(got.data(), want.data(), want.size()) == 0);
        device->destroy(tex);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: a texture can be uploaded again and the new contents win")
{
    const auto device = make_validated_device("upload_twice");
    const rhi::TextureHandle tex = device->create_texture(sampled_desc(8, 8, 1));
    device->upload_texture(tex, pattern(8, 8, 1));
    device->upload_texture(tex, pattern(8, 8, 77));
    const auto got = read_back(*device, tex, 8, 8);
    const auto want = pattern(8, 8, 77);
    CHECK(std::memcmp(got.data(), want.data(), want.size()) == 0);
    device->destroy(tex);
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: invalid uploads are rejected before touching the GPU")
{
    const auto device = make_validated_device("upload_errors");
    const rhi::TextureHandle ok = device->create_texture(sampled_desc(4, 4, 1));
    const auto good = pattern(4, 4, 0);
    CHECK(throws_invalid([&] { device->upload_texture(ok, std::span<const std::byte>(good).first(good.size() - 1)); }));
    CHECK(throws_invalid([&] { device->upload_texture(ok, {}); }));
    auto too_big = good;
    too_big.push_back(std::byte{0});
    CHECK(throws_invalid([&] { device->upload_texture(ok, too_big); }));
    CHECK(throws_invalid([&] { device->upload_texture({}, good); }));

    rhi::TextureDesc no_dst = sampled_desc(4, 4, 1);
    no_dst.usage = rhi::TextureUsage::Sampled;
    const rhi::TextureHandle a = device->create_texture(no_dst);
    CHECK(throws_invalid([&] { device->upload_texture(a, good); }));
    rhi::TextureDesc no_sampled = sampled_desc(4, 4, 1);
    no_sampled.usage = rhi::TextureUsage::TransferDst;
    const rhi::TextureHandle c = device->create_texture(no_sampled);
    CHECK(throws_invalid([&] { device->upload_texture(c, good); }));

    // Not allowed while a frame is being recorded.
    (void)device->begin_frame();
    bool threw_logic = false;
    try {
        device->upload_texture(ok, good);
    } catch (const std::logic_error&) {
        threw_logic = true;
    }
    CHECK(threw_logic);
    (void)device->end_frame();
    device->wait_idle();

    // The failures above must not have corrupted anything: a correct upload still works.
    device->upload_texture(ok, good);
    const auto got = read_back(*device, ok, 4, 4);
    CHECK(std::memcmp(got.data(), good.data(), good.size()) == 0);
    device->destroy(ok);
    device->destroy(a);
    device->destroy(c);
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: samplers cover every filter, address mode and border color and destroy cleanly")
{
    const auto device = make_validated_device("samplers");
    std::vector<rhi::SamplerHandle> samplers;
    for (const rhi::Filter f : {rhi::Filter::Nearest, rhi::Filter::Linear}) {
        for (const rhi::AddressMode m : {rhi::AddressMode::Repeat, rhi::AddressMode::MirroredRepeat,
                                         rhi::AddressMode::ClampToEdge, rhi::AddressMode::ClampToBorder}) {
            for (const rhi::BorderColor border : {rhi::BorderColor::TransparentBlack, rhi::BorderColor::OpaqueWhite}) {
                rhi::SamplerDesc sd;
                sd.min_filter = sd.mag_filter = f;
                sd.address_u = sd.address_v = sd.address_w = m;
                sd.border_color = border;
                sd.max_anisotropy = f == rhi::Filter::Linear ? 8.0f : 1.0f;  // degrades gracefully if unsupported
                samplers.push_back(device->create_sampler(sd));
            }
        }
    }
    rhi::SamplerDesc bad;
    bad.max_anisotropy = 0.5f;
    CHECK(throws_invalid([&] { (void)device->create_sampler(bad); }));
    bad = {};
    bad.min_lod = 5.0f;
    bad.max_lod = 1.0f;
    CHECK(throws_invalid([&] { (void)device->create_sampler(bad); }));
    for (const rhi::SamplerHandle s : samplers) {
        device->destroy(s);
    }
    // Destroying again is reported, not fatal.
    device->destroy(samplers.front());
    (void)device->begin_frame();
    (void)device->end_frame();
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
