// Stage visibility of bind group layout entries, the storage-texture binding, and the per-stage binding totals: what compute pipelines build on, with the graphics defaults unchanged.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/resource_desc.hpp"

#include <vector>

using namespace saige::rhi;

namespace {

BindGroupLayoutEntry entry(uint32_t binding, BindingType type, StageMask visibility = graphics_stages)
{
    BindGroupLayoutEntry e;
    e.binding = binding;
    e.type = type;
    e.visibility = visibility;
    return e;
}

BindGroupLayoutDesc layout_of(std::vector<BindGroupLayoutEntry> entries)
{
    BindGroupLayoutDesc d;
    d.entries = std::move(entries);
    return d;
}

ValidationResult totals(const std::vector<BindGroupLayoutDesc>& layouts, StageMask stages)
{
    std::vector<const BindGroupLayoutDesc*> ptrs;
    for (const auto& l : layouts) ptrs.push_back(&l);
    return validate_pipeline_bindings(ptrs, stages);
}

} // namespace

TEST_CASE("visibility: the default is the two graphics stages, so every layout written before compute existed means what it meant")
{
    static_assert(graphics_stages == (StageMask::Vertex | StageMask::Pixel));
    const BindGroupLayoutEntry e;
    CHECK(e.visibility == graphics_stages);
    const BindGroupLayoutDesc legacy = layout_of({entry(0, BindingType::UniformBuffer), entry(1, BindingType::SampledTexture), entry(2, BindingType::Sampler), entry(3, BindingType::StorageBuffer)});
    CHECK(validate(legacy));
    // A graphics pipeline over only-default layouts is checked exactly as before (the default argument is the graphics stages).
    std::vector<BindGroupLayoutDesc> layouts{legacy};
    std::vector<const BindGroupLayoutDesc*> ptrs{&layouts[0]};
    CHECK(validate_pipeline_bindings(ptrs));
    // Five storage buffers in graphics layouts still exceed the portable total of four.
    std::vector<BindGroupLayoutEntry> five;
    for (uint32_t i = 0; i < 5; ++i) five.push_back(entry(i, BindingType::StorageBuffer));
    CHECK(validate_pipeline_bindings(std::vector<const BindGroupLayoutDesc*>{}));  // a pipeline with no layouts is fine
    std::vector<BindGroupLayoutDesc> over{layout_of(five)};
    CHECK_FALSE(totals(over, graphics_stages));
}

TEST_CASE("visibility: a layout entry needs a non-empty set of known stages, and a storage texture is compute-only")
{
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::UniformBuffer, StageMask::None)})));
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::UniformBuffer, static_cast<StageMask>(8))})));
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::UniformBuffer, static_cast<StageMask>(0x80 | 1))})));
    CHECK(validate(layout_of({entry(0, BindingType::UniformBuffer, StageMask::Compute)})));
    CHECK(validate(layout_of({entry(0, BindingType::StorageBuffer, StageMask::Vertex | StageMask::Compute)})));
    CHECK(validate(layout_of({entry(0, BindingType::SampledTexture, StageMask::Pixel | StageMask::Compute)})));
    CHECK(validate(layout_of({entry(0, BindingType::StorageTexture, StageMask::Compute)})));
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::StorageTexture, graphics_stages)})));
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::StorageTexture, StageMask::Pixel)})));
    CHECK_FALSE(validate(layout_of({entry(0, BindingType::StorageTexture, StageMask::Compute | StageMask::Pixel)})));
    // The older rules still hold alongside the new ones.
    CHECK_FALSE(validate(layout_of({})));
    CHECK_FALSE(validate(layout_of({entry(1, BindingType::UniformBuffer), entry(1, BindingType::Sampler)})));
}

TEST_CASE("visibility: the per-pipeline totals are per stage, so a compute-only binding costs a graphics pipeline nothing and vice versa")
{
    std::vector<BindGroupLayoutEntry> compute_five, graphics_five, both_five;
    for (uint32_t i = 0; i < 5; ++i) {
        compute_five.push_back(entry(i, BindingType::StorageBuffer, StageMask::Compute));
        graphics_five.push_back(entry(i, BindingType::StorageBuffer, graphics_stages));
        both_five.push_back(entry(i, BindingType::StorageBuffer, StageMask::Vertex | StageMask::Compute));
    }
    const std::vector<BindGroupLayoutDesc> c{layout_of(compute_five)}, g{layout_of(graphics_five)}, b{layout_of(both_five)};
    CHECK(totals(c, graphics_stages));            // invisible to the graphics stages: not counted
    CHECK_FALSE(totals(c, StageMask::Compute));   // five storage buffers in the compute stage: over the limit
    CHECK_FALSE(totals(g, graphics_stages));
    CHECK(totals(g, StageMask::Compute));
    CHECK_FALSE(totals(b, graphics_stages));      // visible to both: counts for each
    CHECK_FALSE(totals(b, StageMask::Compute));
    // Four are fine everywhere.
    std::vector<BindGroupLayoutEntry> four;
    for (uint32_t i = 0; i < 4; ++i) four.push_back(entry(i, BindingType::StorageBuffer, StageMask::Vertex | StageMask::Compute));
    const std::vector<BindGroupLayoutDesc> f{layout_of(four)};
    CHECK(totals(f, graphics_stages));
    CHECK(totals(f, StageMask::Compute));
    // The limit is over all of the pipeline's layouts together, not per layout.
    std::vector<BindGroupLayoutEntry> three_a, three_b;
    for (uint32_t i = 0; i < 3; ++i) {
        three_a.push_back(entry(i, BindingType::StorageBuffer, StageMask::Compute));
        three_b.push_back(entry(i, BindingType::StorageBuffer, StageMask::Compute));
    }
    CHECK_FALSE(totals({layout_of(three_a), layout_of(three_b)}, StageMask::Compute));
}

TEST_CASE("visibility: at most four storage textures per pipeline, and only compute pipelines count them")
{
    std::vector<BindGroupLayoutEntry> four, five;
    for (uint32_t i = 0; i < 5; ++i) {
        if (i < 4) four.push_back(entry(i, BindingType::StorageTexture, StageMask::Compute));
        five.push_back(entry(i, BindingType::StorageTexture, StageMask::Compute));
    }
    CHECK(totals({layout_of(four)}, StageMask::Compute));
    CHECK_FALSE(totals({layout_of(five)}, StageMask::Compute));
    CHECK(totals({layout_of(five)}, graphics_stages));  // compute-only entries do not count against a graphics pipeline
    static_assert(max_pipeline_storage_textures == 4);
}

TEST_CASE("bind groups: a storage texture binding needs a live single-sample texture with Storage usage")
{
    const BindGroupLayoutDesc layout = layout_of({entry(0, BindingType::StorageTexture, StageMask::Compute)});
    TextureDesc storage;
    storage.format = Format::RGBA8_UNORM;
    storage.width = storage.height = 8;
    storage.usage = TextureUsage::Storage | TextureUsage::Sampled;
    TextureDesc sampled_only = storage;
    sampled_only.usage = TextureUsage::Sampled;
    TextureDesc msaa = storage;
    msaa.sample_count = 4;
    TextureHandle ok, plain, ms, dead;
    // Handles are opaque; the resolver below maps them by identity of a small table.
    const std::vector<TextureDesc> table{storage, sampled_only, msaa};
    BindGroupResolver r;
    r.texture = [&](TextureHandle h) -> std::optional<TextureDesc> {
        if (h == ok) return table[0];
        if (h == plain) return table[1];
        if (h == ms) return table[2];
        return std::nullopt;
    };
    r.sampler_exists = [](SamplerHandle) { return false; };
    r.buffer = [](BufferHandle) -> std::optional<BufferDesc> { return std::nullopt; };
    // Distinct handles from a throwaway pool-free construction: handles compare by value, so craft three different ones.
    ok = TextureHandle::from_raw(1);
    plain = TextureHandle::from_raw(2);
    ms = TextureHandle::from_raw(3);
    dead = TextureHandle::from_raw(4);
    auto entries_for = [](TextureHandle h) {
        BindGroupEntry e;
        e.binding = 0;
        e.texture = h;
        return std::vector<BindGroupEntry>{e};
    };
    CHECK(validate(layout, entries_for(ok), r));
    CHECK_FALSE(validate(layout, entries_for(plain), r));
    CHECK_FALSE(validate(layout, entries_for(ms), r));
    CHECK_FALSE(validate(layout, entries_for(dead), r));
}
