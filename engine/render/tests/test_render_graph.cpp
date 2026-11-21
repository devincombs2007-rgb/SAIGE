#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/render_graph.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::render;
using rhi::Access;
using rhi::PipelineStage;
using rhi::ResourceState;
using rhi::TextureLayout;

namespace {

constexpr PipelineStage PS = PipelineStage::PixelShader;
constexpr PipelineStage CS = PipelineStage::ComputeShader;

rhi::TextureDesc tex_2d(rhi::Format format, uint32_t w = 1920, uint32_t h = 1080)
{
    rhi::TextureDesc d;
    d.format = format;
    d.width = w;
    d.height = h;
    return d;  // usage left as None: inferred by the graph
}

rhi::TextureDesc backbuffer_desc()
{
    rhi::TextureDesc d = tex_2d(rhi::Format::BGRA8_SRGB);
    d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferDst;
    return d;
}

const ResourceState acquired{PipelineStage::ColorAttachmentOutput, Access::None, TextureLayout::Undefined};
const ResourceState present{PipelineStage::None, Access::None, TextureLayout::Present};

const GraphBarrier* find_barrier(const CompiledPass& pass, uint32_t resource)
{
    for (const GraphBarrier& b : pass.barriers_before) {
        if (b.resource == resource) {
            return &b;
        }
    }
    return nullptr;
}

bool contains(const std::vector<uint32_t>& v, uint32_t x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

/// GBuffer -> Lighting -> Tonemap -> Backbuffer. A minimal deferred frame.
struct DeferredFrame {
    RenderGraph graph;
    RGTexture backbuffer, gbuffer, depth, hdr;

    DeferredFrame()
    {
        backbuffer = graph.import_texture("Backbuffer", backbuffer_desc(), rhi::TextureHandle::from_parts(7, 1), acquired,
                                         present);
        graph.add_pass("GBuffer", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            gbuffer = b.create_texture("GBufferA", tex_2d(rhi::Format::RGBA8_UNORM));
            depth = b.create_texture("Depth", tex_2d(rhi::Format::D32_FLOAT));
            b.write_color(gbuffer, LoadOp::Clear);
            b.write_depth(depth, LoadOp::Clear);
        });
        graph.add_pass("Lighting", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            hdr = b.create_texture("HDR", tex_2d(rhi::Format::RGBA16_FLOAT));
            b.sample(gbuffer, PS);
            b.read_depth(depth);
            b.write_color(hdr, LoadOp::Clear);
        });
        graph.add_pass("Tonemap", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            b.sample(hdr, PS);
            b.write_color(backbuffer, LoadOp::DontCare);
        });
    }
};

} // namespace

// =====================================================================================================================
// Culling
// =====================================================================================================================

TEST_CASE("RenderGraph: deferred frame compiles with no culling")
{
    DeferredFrame f;
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.size() == 3);
    CHECK(r.culled_passes.empty());
    CHECK(r.warnings.empty());
}

TEST_CASE("RenderGraph: passes whose outputs are never consumed are culled")
{
    DeferredFrame f;
    RGTexture debug_tex;
    const uint32_t debug_pass = f.graph.add_pass("DebugViz", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        debug_tex = b.create_texture("Debug", tex_2d(rhi::Format::RGBA8_UNORM));
        b.sample(f.hdr, PS);
        b.write_color(debug_tex, LoadOp::Clear);
    });
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);
    CHECK(contains(r.culled_passes, debug_pass));
    CHECK(r.passes.size() == 3);
    CHECK_FALSE(r.lifetimes[debug_tex.id].used());
}

TEST_CASE("RenderGraph: culling propagates through unused chains")
{
    RenderGraph g;
    RGTexture x, y;
    g.add_pass("A", rhi::QueueType::Compute, [&](PassBuilder& b) {
        x = b.create_texture("X", tex_2d(rhi::Format::R32_FLOAT, 64, 64));
        b.write_storage(x, CS, true);
    });
    g.add_pass("B", rhi::QueueType::Compute, [&](PassBuilder& b) {
        y = b.create_texture("Y", tex_2d(rhi::Format::R32_FLOAT, 64, 64));
        b.read_storage(x, CS);
        b.write_storage(y, CS, true);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.empty());
    CHECK(r.culled_passes.size() == 2);
}

TEST_CASE("RenderGraph: side-effect passes are never culled, and keep their producers alive")
{
    RenderGraph g;
    RGBuffer stats;
    g.add_pass("Produce", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 256;
        stats = b.create_buffer("Stats", d);
        b.write_storage(stats, CS, true);
    });
    g.add_pass("Readback", rhi::QueueType::Transfer, [&](PassBuilder& b) {
        b.copy_src(stats);
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.size() == 2);
    CHECK(r.culled_passes.empty());
}

TEST_CASE("RenderGraph: a full overwrite (Clear) removes the dependency on earlier writers")
{
    RenderGraph g;
    RGTexture bb = g.import_texture("BB", backbuffer_desc(), {}, acquired, present);
    RGTexture rt;
    const uint32_t a = g.add_pass("A", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.create_texture("RT", tex_2d(rhi::Format::RGBA8_UNORM));
        b.write_color(rt, LoadOp::Clear);
    });
    const uint32_t b_pass = g.add_pass("B", rhi::QueueType::Graphics, [&](PassBuilder& b) { b.write_color(rt, LoadOp::Clear); });
    g.add_pass("C", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.write_color(bb, LoadOp::DontCare);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(contains(r.culled_passes, a));
    CHECK_FALSE(contains(r.culled_passes, b_pass));
}

TEST_CASE("RenderGraph: LoadOp::Load keeps the earlier writer alive (read-modify-write)")
{
    RenderGraph g;
    RGTexture bb = g.import_texture("BB", backbuffer_desc(), {}, acquired, present);
    RGTexture rt;
    g.add_pass("Opaque", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.create_texture("RT", tex_2d(rhi::Format::RGBA16_FLOAT));
        b.write_color(rt, LoadOp::Clear);
    });
    g.add_pass("Transparent", rhi::QueueType::Graphics, [&](PassBuilder& b) { b.write_color(rt, LoadOp::Load); });
    g.add_pass("Resolve", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.write_color(bb, LoadOp::DontCare);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.size() == 3);
}

// =====================================================================================================================
// Barriers
// =====================================================================================================================

TEST_CASE("RenderGraph: barriers for the deferred frame follow sync2 semantics")
{
    DeferredFrame f;
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);

    // GBuffer: first use, Undefined -> attachment, nothing to wait for.
    const GraphBarrier* g0 = find_barrier(r.passes[0], f.gbuffer.id);
    REQUIRE(g0 != nullptr);
    CHECK(g0->before.layout == TextureLayout::Undefined);
    CHECK(g0->before.stages == PipelineStage::None);
    CHECK(g0->after.layout == TextureLayout::ColorAttachment);

    // Lighting: GBuffer attachment write -> pixel shader sampled read.
    const GraphBarrier* g1 = find_barrier(r.passes[1], f.gbuffer.id);
    REQUIRE(g1 != nullptr);
    CHECK(g1->before.layout == TextureLayout::ColorAttachment);
    CHECK(rhi::has_any(g1->before.stages, PipelineStage::ColorAttachmentOutput));
    CHECK(g1->before.access == Access::ColorAttachmentWrite);
    CHECK(g1->after.layout == TextureLayout::ShaderReadOnly);
    CHECK(g1->after.stages == PS);

    // Depth becomes read-only depth.
    const GraphBarrier* d1 = find_barrier(r.passes[1], f.depth.id);
    REQUIRE(d1 != nullptr);
    CHECK(d1->before.layout == TextureLayout::DepthStencilAttachment);
    CHECK(d1->after.layout == TextureLayout::DepthStencilReadOnly);
    CHECK(d1->before.access == Access::DepthStencilWrite);

    // Tonemap: backbuffer content discarded on first write.
    const GraphBarrier* bb = find_barrier(r.passes[2], f.backbuffer.id);
    REQUIRE(bb != nullptr);
    CHECK(bb->before.layout == TextureLayout::Undefined);
    CHECK(rhi::has_any(bb->before.stages, PipelineStage::ColorAttachmentOutput));  // waits on swapchain acquire

    // Final: backbuffer -> Present.
    REQUIRE(r.final_barriers.size() == 1);
    CHECK(r.final_barriers[0].resource == f.backbuffer.id);
    CHECK(r.final_barriers[0].before.layout == TextureLayout::ColorAttachment);
    CHECK(r.final_barriers[0].before.access == Access::ColorAttachmentWrite);
    CHECK(r.final_barriers[0].after.layout == TextureLayout::Present);
}

TEST_CASE("RenderGraph: read-after-read needs no barrier")
{
    RenderGraph g;
    rhi::TextureDesc env_desc = tex_2d(rhi::Format::BC6H_UFLOAT, 512, 512);
    env_desc.usage = rhi::TextureUsage::Sampled;
    const ResourceState srv{PS, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly};
    RGTexture env = g.import_texture("EnvMap", env_desc, {}, srv);
    RGBuffer out;
    g.add_pass("P1", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(env, PS);
        b.set_side_effects();
    });
    g.add_pass("P2", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 64;
        out = b.create_buffer("Out", d);
        b.sample(env, CS);  // different stage, but no writes: still no barrier
        b.write_storage(out, CS, true);
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(find_barrier(r.passes[0], env.id) == nullptr);
    CHECK(find_barrier(r.passes[1], env.id) == nullptr);
}

TEST_CASE("RenderGraph: reads in a new stage after a write chain through prior readers")
{
    RenderGraph g;
    RGTexture rt;
    g.add_pass("Write", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.create_texture("RT", tex_2d(rhi::Format::RGBA16_FLOAT));
        b.write_color(rt, LoadOp::Clear);
    });
    g.add_pass("ReadPS", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.set_side_effects();
    });
    g.add_pass("ReadPSAgain", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.set_side_effects();
    });
    g.add_pass("ReadCS", rhi::QueueType::Compute, [&](PassBuilder& b) {
        b.sample(rt, CS);
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    REQUIRE(r.passes.size() == 4);
    CHECK(find_barrier(r.passes[1], rt.id) != nullptr);  // layout transition
    CHECK(find_barrier(r.passes[2], rt.id) == nullptr);  // same stage, already visible
    const GraphBarrier* cs = find_barrier(r.passes[3], rt.id);
    REQUIRE(cs != nullptr);
    CHECK(cs->before.layout == TextureLayout::ShaderReadOnly);
    CHECK(cs->after.layout == TextureLayout::ShaderReadOnly);
    CHECK(rhi::has_all(cs->before.stages, PipelineStage::ColorAttachmentOutput | PS));
    CHECK(cs->before.access == Access::ColorAttachmentWrite);
}

TEST_CASE("RenderGraph: storage write after storage write emits a UAV barrier")
{
    RenderGraph g;
    rhi::TextureDesc hist_desc = tex_2d(rhi::Format::RGBA16_FLOAT, 256, 256);
    hist_desc.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
    const ResourceState general{CS, Access::ShaderStorageRead, TextureLayout::General};
    RGTexture hist = g.import_texture("History", hist_desc, {}, general, general);
    g.add_pass("Pass1", rhi::QueueType::Compute, [&](PassBuilder& b) { b.write_storage(hist, CS); });
    g.add_pass("Pass2", rhi::QueueType::Compute, [&](PassBuilder& b) { b.write_storage(hist, CS); });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    const GraphBarrier* uav = find_barrier(r.passes[1], hist.id);
    REQUIRE(uav != nullptr);
    CHECK(uav->before.stages == CS);
    CHECK(uav->before.access == Access::ShaderStorageWrite);
    CHECK(uav->before.layout == TextureLayout::General);

    // Leaving the graph: the pending write must be made visible to the final (read) state.
    REQUIRE(r.final_barriers.size() == 1);
    CHECK(r.final_barriers[0].before.access == Access::ShaderStorageWrite);
}

TEST_CASE("RenderGraph: GPU-driven buffer chain (compute writes indirect args, draw consumes them)")
{
    RenderGraph g;
    RGTexture bb = g.import_texture("BB", backbuffer_desc(), {}, acquired, present);
    RGBuffer args;
    g.add_pass("Cull", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 4096;
        args = b.create_buffer("DrawArgs", d);
        b.write_storage(args, CS, true);
    });
    g.add_pass("Draw", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.read_buffer(args, BufferReadKind::Indirect);
        b.write_color(bb, LoadOp::Clear);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    const GraphBarrier* br = find_barrier(r.passes[1], args.id);
    REQUIRE(br != nullptr);
    CHECK(br->before.stages == CS);
    CHECK(br->before.access == Access::ShaderStorageWrite);
    CHECK(br->after.stages == PipelineStage::DrawIndirect);
    CHECK(br->after.access == Access::IndirectCommandRead);
    CHECK(rhi::has_all(g.buffer_desc(args).usage, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect));
}

// =====================================================================================================================
// Usage inference and validation
// =====================================================================================================================

TEST_CASE("RenderGraph: transient usage flags are inferred from accesses")
{
    DeferredFrame f;
    REQUIRE(f.graph.compile().ok);
    CHECK(f.graph.texture_desc(f.gbuffer).usage == (rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled));
    CHECK(f.graph.texture_desc(f.depth).usage == rhi::TextureUsage::DepthStencilAttachment);
}

TEST_CASE("RenderGraph: depth read + sampling in one pass merges to DepthStencilReadOnly")
{
    DeferredFrame f;
    f.graph.add_pass("SoftParticles", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.read_depth(f.depth);
        b.sample(f.depth, PS);
        b.write_color(f.backbuffer, LoadOp::Load);
    });
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);
    CHECK(rhi::has_all(f.graph.texture_desc(f.depth).usage,
                      rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled));
}

TEST_CASE("RenderGraph: sampling an attachment of the same pass is an error (feedback loop)")
{
    RenderGraph g;
    g.add_pass("Bad", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.create_texture("T", tex_2d(rhi::Format::RGBA8_UNORM));
        b.write_color(t, LoadOp::Clear);
        b.sample(t, PS);
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("RenderGraph: imported textures must allow the declared accesses")
{
    RenderGraph g;
    rhi::TextureDesc d = tex_2d(rhi::Format::RGBA8_UNORM);
    d.usage = rhi::TextureUsage::ColorAttachment;  // not Sampled
    RGTexture t = g.import_texture("NotSampleable", d, {}, {});
    g.add_pass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(t, PS);
        b.set_side_effects();
    });
    CHECK_FALSE(g.compile().ok);
}

TEST_CASE("RenderGraph: invalid transient descriptions are reported")
{
    RenderGraph g;
    g.add_pass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.create_texture("Compressed", tex_2d(rhi::Format::BC7_UNORM, 256, 256));
        b.write_color(t, LoadOp::Clear);  // can't render to BC7
        b.set_side_effects();
    });
    CHECK_FALSE(g.compile().ok);
}

TEST_CASE("RenderGraph: invalid handles and wrong resource kinds are reported")
{
    RenderGraph g;
    RGBuffer buf;
    g.add_pass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(RGTexture{}, PS);
        rhi::BufferDesc d;
        d.size = 16;
        buf = b.create_buffer("B", d);
        b.sample(RGTexture{buf.id}, PS);  // a buffer id used as a texture
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    CHECK_FALSE(r.ok);
    CHECK(r.errors.size() == 2);
}

TEST_CASE("RenderGraph: reading a never-written transient produces a warning")
{
    RenderGraph g;
    g.add_pass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.create_texture("Garbage", tex_2d(rhi::Format::RGBA8_UNORM));
        b.sample(t, PS);
        b.set_side_effects();
    });
    const CompileResult& r = g.compile();
    CHECK(r.ok);
    CHECK(r.warnings.size() == 1);
}

// =====================================================================================================================
// Transient aliasing
// =====================================================================================================================

namespace {

/// A -> T1 -> B -> T2 -> C -> T3 -> D -> backbuffer (post-process chain).
struct PostChain {
    RenderGraph graph;
    RGTexture bb, t1, t2, t3;
    RGBuffer scratch;

    PostChain()
    {
        bb = graph.import_texture("BB", backbuffer_desc(), {}, acquired, present);
        const rhi::TextureDesc d = tex_2d(rhi::Format::RGBA16_FLOAT, 1024, 1024);
        graph.add_pass("A", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            t1 = b.create_texture("T1", d);
            b.write_color(t1, LoadOp::Clear);
        });
        graph.add_pass("B", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            t2 = b.create_texture("T2", d);
            b.sample(t1, PS);
            b.write_color(t2, LoadOp::Clear);
        });
        graph.add_pass("C", rhi::QueueType::Compute, [&](PassBuilder& b) {
            t3 = b.create_texture("T3", d);
            rhi::BufferDesc bd;
            bd.size = 1u << 20;
            scratch = b.create_buffer("Scratch", bd);
            b.sample(t2, CS);
            b.write_storage(scratch, CS, true);
            b.write_storage(t3, CS, true);
        });
        graph.add_pass("D", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            b.sample(t3, PS);
            b.write_color(bb, LoadOp::DontCare);
        });
    }
};

} // namespace

TEST_CASE("RenderGraph: non-overlapping transients share memory")
{
    PostChain c;
    const CompileResult& r = c.graph.compile();
    REQUIRE(r.ok);

    // Lifetimes (execution positions): T1 [0,1], T2 [1,2], T3 [2,3]
    CHECK(r.lifetimes[c.t1.id].first_pass == 0);
    CHECK(r.lifetimes[c.t1.id].last_pass == 1);
    CHECK(r.lifetimes[c.t3.id].first_pass == 2);

    CHECK(r.allocations[c.t3.id].slot == r.allocations[c.t1.id].slot);  // T3 reuses T1's memory
    CHECK(r.allocations[c.t3.id].alias_predecessor == c.t1.id);
    CHECK(r.allocations[c.t2.id].slot != r.allocations[c.t1.id].slot);  // overlapping lifetimes
    CHECK(r.allocations[c.scratch.id].slot != r.allocations[c.t1.id].slot);  // buffers never alias textures
    CHECK(r.allocations[c.bb.id].slot == invalid_resource);  // imported: not managed

    CHECK(r.transient_bytes_aliased < r.transient_bytes_unaliased);
    CHECK(r.slot_sizes.size() == 3);  // {T1/T3, T2, Scratch}
}

TEST_CASE("RenderGraph: first use of an aliased resource waits for the previous occupant")
{
    PostChain c;
    const CompileResult& r = c.graph.compile();
    REQUIRE(r.ok);
    const GraphBarrier* b = find_barrier(r.passes[2], c.t3.id);
    REQUIRE(b != nullptr);
    CHECK(b->before.layout == TextureLayout::Undefined);
    CHECK(rhi::has_any(b->before.stages, PS));  // T1 was last sampled in the pixel shader (pass B)
}

TEST_CASE("RenderGraph: aliasing can be disabled, and custom size callbacks are honored")
{
    PostChain c;
    CompileOptions opts;
    opts.enable_aliasing = false;
    opts.texture_size = [](const rhi::TextureDesc&) -> uint64_t { return 1000; };
    opts.buffer_size = [](const rhi::BufferDesc&) -> uint64_t { return 10; };
    const CompileResult& r = c.graph.compile(opts);
    REQUIRE(r.ok);
    CHECK(r.slot_sizes.size() == 4);
    CHECK(r.transient_bytes_unaliased == 3010);
    CHECK(r.transient_bytes_aliased == 3010);
}

// =====================================================================================================================
// Execution
// =====================================================================================================================

TEST_CASE("RenderGraph: execute runs surviving passes in order with resolved barriers")
{
    RenderGraph g;
    std::vector<std::string> log;
    const auto bb_handle = rhi::TextureHandle::from_parts(42, 3);
    RGTexture bb = g.import_texture("BB", backbuffer_desc(), bb_handle, acquired, present);
    RGTexture rt;
    rhi::TextureHandle seen_transient;

    g.add_pass(
        "Draw", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
            rt = b.create_texture("RT", tex_2d(rhi::Format::RGBA8_UNORM));
            b.write_color(rt, LoadOp::Clear);
        },
        [&](PassContext& ctx) {
            log.push_back(std::string(ctx.pass_name()));
            seen_transient = ctx.texture(rt);
            CHECK(ctx.command_list() == nullptr);
        });
    g.add_pass(
        "Unused", rhi::QueueType::Graphics,
        [&](PassBuilder& b) { b.write_color(b.create_texture("X", tex_2d(rhi::Format::RGBA8_UNORM)), LoadOp::Clear); },
        [&](PassContext&) { log.push_back("Unused"); });
    g.add_pass(
        "Blit", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
            b.sample(rt, PS);
            b.write_color(bb, LoadOp::DontCare);
        },
        [&](PassContext& ctx) {
            log.push_back(std::string(ctx.pass_name()));
            CHECK(ctx.texture(bb) == bb_handle);
            CHECK(ctx.texture_desc(bb).format == rhi::Format::BGRA8_SRGB);
        });

    REQUIRE(g.compile().ok);

    std::vector<rhi::TextureHandle> physical(g.resource_count());
    const auto rt_handle = rhi::TextureHandle::from_parts(5, 1);
    physical[rt.id] = rt_handle;

    std::vector<rhi::TextureBarrier> submitted;
    ExecuteContext ctx;
    ctx.transient_textures = physical;
    ctx.submit_barriers = [&](std::span<const rhi::TextureBarrier> tex, std::span<const rhi::BufferBarrier> buf) {
        submitted.insert(submitted.end(), tex.begin(), tex.end());
        CHECK(buf.empty());
    };
    g.execute(ctx);

    REQUIRE(log.size() == 2);
    CHECK(log[0] == "Draw");
    CHECK(log[1] == "Blit");
    CHECK(seen_transient == rt_handle);

    // Draw: RT init. Blit: RT -> SRV, BB init. Final: BB -> Present.
    REQUIRE(submitted.size() == 4);
    CHECK(submitted[0].texture == rt_handle);
    CHECK(submitted.back().texture == bb_handle);
    CHECK(submitted.back().after.layout == TextureLayout::Present);
}

TEST_CASE("RenderGraph: graphviz output lists passes and marks culled ones")
{
    DeferredFrame f;
    f.graph.add_pass("Orphan", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.write_color(b.create_texture("Lonely", tex_2d(rhi::Format::RGBA8_UNORM)), LoadOp::Clear);
    });
    f.graph.compile();
    const std::string dot = f.graph.to_graphviz();
    CHECK(dot.find("digraph RenderGraph") != std::string::npos);
    CHECK(dot.find("GBuffer") != std::string::npos);
    CHECK(dot.find("Backbuffer") != std::string::npos);
    CHECK(dot.find("dashed") != std::string::npos);
}

TEST_CASE("RenderGraph: reset clears everything for the next frame")
{
    DeferredFrame f;
    REQUIRE(f.graph.compile().ok);
    f.graph.reset();
    CHECK(f.graph.pass_count() == 0);
    CHECK(f.graph.resource_count() == 0);
    const CompileResult& r = f.graph.compile();
    CHECK(r.ok);
    CHECK(r.passes.empty());
}
