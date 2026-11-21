#include "saige/gfx_test/saige_test.h"

#include "saige/render/render_graph.h"

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

rhi::TextureDesc tex2D(rhi::Format format, uint32_t w = 1920, uint32_t h = 1080)
{
    rhi::TextureDesc d;
    d.format = format;
    d.width = w;
    d.height = h;
    return d;  // usage left as None: inferred by the graph
}

rhi::TextureDesc backbufferDesc()
{
    rhi::TextureDesc d = tex2D(rhi::Format::BGRA8_SRGB);
    d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferDst;
    return d;
}

const ResourceState kAcquired{PipelineStage::ColorAttachmentOutput, Access::None, TextureLayout::Undefined};
const ResourceState kPresent{PipelineStage::None, Access::None, TextureLayout::Present};

const GraphBarrier* findBarrier(const CompiledPass& pass, uint32_t resource)
{
    for (const GraphBarrier& b : pass.barriersBefore) {
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
        backbuffer = graph.importTexture("Backbuffer", backbufferDesc(), rhi::TextureHandle::fromParts(7, 1), kAcquired,
                                         kPresent);
        graph.addPass("GBuffer", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            gbuffer = b.createTexture("GBufferA", tex2D(rhi::Format::RGBA8_UNORM));
            depth = b.createTexture("Depth", tex2D(rhi::Format::D32_FLOAT));
            b.writeColor(gbuffer, LoadOp::Clear);
            b.writeDepth(depth, LoadOp::Clear);
        });
        graph.addPass("Lighting", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            hdr = b.createTexture("HDR", tex2D(rhi::Format::RGBA16_FLOAT));
            b.sample(gbuffer, PS);
            b.readDepth(depth);
            b.writeColor(hdr, LoadOp::Clear);
        });
        graph.addPass("Tonemap", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            b.sample(hdr, PS);
            b.writeColor(backbuffer, LoadOp::DontCare);
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
    CHECK(r.culledPasses.empty());
    CHECK(r.warnings.empty());
}

TEST_CASE("RenderGraph: passes whose outputs are never consumed are culled")
{
    DeferredFrame f;
    RGTexture debugTex;
    const uint32_t debugPass = f.graph.addPass("DebugViz", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        debugTex = b.createTexture("Debug", tex2D(rhi::Format::RGBA8_UNORM));
        b.sample(f.hdr, PS);
        b.writeColor(debugTex, LoadOp::Clear);
    });
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);
    CHECK(contains(r.culledPasses, debugPass));
    CHECK(r.passes.size() == 3);
    CHECK_FALSE(r.lifetimes[debugTex.id].used());
}

TEST_CASE("RenderGraph: culling propagates through unused chains")
{
    RenderGraph g;
    RGTexture x, y;
    g.addPass("A", rhi::QueueType::Compute, [&](PassBuilder& b) {
        x = b.createTexture("X", tex2D(rhi::Format::R32_FLOAT, 64, 64));
        b.writeStorage(x, CS, true);
    });
    g.addPass("B", rhi::QueueType::Compute, [&](PassBuilder& b) {
        y = b.createTexture("Y", tex2D(rhi::Format::R32_FLOAT, 64, 64));
        b.readStorage(x, CS);
        b.writeStorage(y, CS, true);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.empty());
    CHECK(r.culledPasses.size() == 2);
}

TEST_CASE("RenderGraph: side-effect passes are never culled, and keep their producers alive")
{
    RenderGraph g;
    RGBuffer stats;
    g.addPass("Produce", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 256;
        stats = b.createBuffer("Stats", d);
        b.writeStorage(stats, CS, true);
    });
    g.addPass("Readback", rhi::QueueType::Transfer, [&](PassBuilder& b) {
        b.copySrc(stats);
        b.setSideEffects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(r.passes.size() == 2);
    CHECK(r.culledPasses.empty());
}

TEST_CASE("RenderGraph: a full overwrite (Clear) removes the dependency on earlier writers")
{
    RenderGraph g;
    RGTexture bb = g.importTexture("BB", backbufferDesc(), {}, kAcquired, kPresent);
    RGTexture rt;
    const uint32_t a = g.addPass("A", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.createTexture("RT", tex2D(rhi::Format::RGBA8_UNORM));
        b.writeColor(rt, LoadOp::Clear);
    });
    const uint32_t bPass = g.addPass("B", rhi::QueueType::Graphics, [&](PassBuilder& b) { b.writeColor(rt, LoadOp::Clear); });
    g.addPass("C", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.writeColor(bb, LoadOp::DontCare);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(contains(r.culledPasses, a));
    CHECK_FALSE(contains(r.culledPasses, bPass));
}

TEST_CASE("RenderGraph: LoadOp::Load keeps the earlier writer alive (read-modify-write)")
{
    RenderGraph g;
    RGTexture bb = g.importTexture("BB", backbufferDesc(), {}, kAcquired, kPresent);
    RGTexture rt;
    g.addPass("Opaque", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.createTexture("RT", tex2D(rhi::Format::RGBA16_FLOAT));
        b.writeColor(rt, LoadOp::Clear);
    });
    g.addPass("Transparent", rhi::QueueType::Graphics, [&](PassBuilder& b) { b.writeColor(rt, LoadOp::Load); });
    g.addPass("Resolve", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.writeColor(bb, LoadOp::DontCare);
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
    const GraphBarrier* g0 = findBarrier(r.passes[0], f.gbuffer.id);
    REQUIRE(g0 != nullptr);
    CHECK(g0->before.layout == TextureLayout::Undefined);
    CHECK(g0->before.stages == PipelineStage::None);
    CHECK(g0->after.layout == TextureLayout::ColorAttachment);

    // Lighting: GBuffer attachment write -> pixel shader sampled read.
    const GraphBarrier* g1 = findBarrier(r.passes[1], f.gbuffer.id);
    REQUIRE(g1 != nullptr);
    CHECK(g1->before.layout == TextureLayout::ColorAttachment);
    CHECK(rhi::hasAny(g1->before.stages, PipelineStage::ColorAttachmentOutput));
    CHECK(g1->before.access == Access::ColorAttachmentWrite);
    CHECK(g1->after.layout == TextureLayout::ShaderReadOnly);
    CHECK(g1->after.stages == PS);

    // Depth becomes read-only depth.
    const GraphBarrier* d1 = findBarrier(r.passes[1], f.depth.id);
    REQUIRE(d1 != nullptr);
    CHECK(d1->before.layout == TextureLayout::DepthStencilAttachment);
    CHECK(d1->after.layout == TextureLayout::DepthStencilReadOnly);
    CHECK(d1->before.access == Access::DepthStencilWrite);

    // Tonemap: backbuffer content discarded on first write.
    const GraphBarrier* bb = findBarrier(r.passes[2], f.backbuffer.id);
    REQUIRE(bb != nullptr);
    CHECK(bb->before.layout == TextureLayout::Undefined);
    CHECK(rhi::hasAny(bb->before.stages, PipelineStage::ColorAttachmentOutput));  // waits on swapchain acquire

    // Final: backbuffer -> Present.
    REQUIRE(r.finalBarriers.size() == 1);
    CHECK(r.finalBarriers[0].resource == f.backbuffer.id);
    CHECK(r.finalBarriers[0].before.layout == TextureLayout::ColorAttachment);
    CHECK(r.finalBarriers[0].before.access == Access::ColorAttachmentWrite);
    CHECK(r.finalBarriers[0].after.layout == TextureLayout::Present);
}

TEST_CASE("RenderGraph: read-after-read needs no barrier")
{
    RenderGraph g;
    rhi::TextureDesc envDesc = tex2D(rhi::Format::BC6H_UFLOAT, 512, 512);
    envDesc.usage = rhi::TextureUsage::Sampled;
    const ResourceState srv{PS, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly};
    RGTexture env = g.importTexture("EnvMap", envDesc, {}, srv);
    RGBuffer out;
    g.addPass("P1", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(env, PS);
        b.setSideEffects();
    });
    g.addPass("P2", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 64;
        out = b.createBuffer("Out", d);
        b.sample(env, CS);  // different stage, but no writes: still no barrier
        b.writeStorage(out, CS, true);
        b.setSideEffects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    CHECK(findBarrier(r.passes[0], env.id) == nullptr);
    CHECK(findBarrier(r.passes[1], env.id) == nullptr);
}

TEST_CASE("RenderGraph: reads in a new stage after a write chain through prior readers")
{
    RenderGraph g;
    RGTexture rt;
    g.addPass("Write", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        rt = b.createTexture("RT", tex2D(rhi::Format::RGBA16_FLOAT));
        b.writeColor(rt, LoadOp::Clear);
    });
    g.addPass("ReadPS", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.setSideEffects();
    });
    g.addPass("ReadPSAgain", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(rt, PS);
        b.setSideEffects();
    });
    g.addPass("ReadCS", rhi::QueueType::Compute, [&](PassBuilder& b) {
        b.sample(rt, CS);
        b.setSideEffects();
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    REQUIRE(r.passes.size() == 4);
    CHECK(findBarrier(r.passes[1], rt.id) != nullptr);  // layout transition
    CHECK(findBarrier(r.passes[2], rt.id) == nullptr);  // same stage, already visible
    const GraphBarrier* cs = findBarrier(r.passes[3], rt.id);
    REQUIRE(cs != nullptr);
    CHECK(cs->before.layout == TextureLayout::ShaderReadOnly);
    CHECK(cs->after.layout == TextureLayout::ShaderReadOnly);
    CHECK(rhi::hasAll(cs->before.stages, PipelineStage::ColorAttachmentOutput | PS));
    CHECK(cs->before.access == Access::ColorAttachmentWrite);
}

TEST_CASE("RenderGraph: storage write after storage write emits a UAV barrier")
{
    RenderGraph g;
    rhi::TextureDesc histDesc = tex2D(rhi::Format::RGBA16_FLOAT, 256, 256);
    histDesc.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
    const ResourceState general{CS, Access::ShaderStorageRead, TextureLayout::General};
    RGTexture hist = g.importTexture("History", histDesc, {}, general, general);
    g.addPass("Pass1", rhi::QueueType::Compute, [&](PassBuilder& b) { b.writeStorage(hist, CS); });
    g.addPass("Pass2", rhi::QueueType::Compute, [&](PassBuilder& b) { b.writeStorage(hist, CS); });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    const GraphBarrier* uav = findBarrier(r.passes[1], hist.id);
    REQUIRE(uav != nullptr);
    CHECK(uav->before.stages == CS);
    CHECK(uav->before.access == Access::ShaderStorageWrite);
    CHECK(uav->before.layout == TextureLayout::General);

    // Leaving the graph: the pending write must be made visible to the final (read) state.
    REQUIRE(r.finalBarriers.size() == 1);
    CHECK(r.finalBarriers[0].before.access == Access::ShaderStorageWrite);
}

TEST_CASE("RenderGraph: GPU-driven buffer chain (compute writes indirect args, draw consumes them)")
{
    RenderGraph g;
    RGTexture bb = g.importTexture("BB", backbufferDesc(), {}, kAcquired, kPresent);
    RGBuffer args;
    g.addPass("Cull", rhi::QueueType::Compute, [&](PassBuilder& b) {
        rhi::BufferDesc d;
        d.size = 4096;
        args = b.createBuffer("DrawArgs", d);
        b.writeStorage(args, CS, true);
    });
    g.addPass("Draw", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.readBuffer(args, BufferReadKind::Indirect);
        b.writeColor(bb, LoadOp::Clear);
    });
    const CompileResult& r = g.compile();
    REQUIRE(r.ok);
    const GraphBarrier* br = findBarrier(r.passes[1], args.id);
    REQUIRE(br != nullptr);
    CHECK(br->before.stages == CS);
    CHECK(br->before.access == Access::ShaderStorageWrite);
    CHECK(br->after.stages == PipelineStage::DrawIndirect);
    CHECK(br->after.access == Access::IndirectCommandRead);
    CHECK(rhi::hasAll(g.bufferDesc(args).usage, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect));
}

// =====================================================================================================================
// Usage inference and validation
// =====================================================================================================================

TEST_CASE("RenderGraph: transient usage flags are inferred from accesses")
{
    DeferredFrame f;
    REQUIRE(f.graph.compile().ok);
    CHECK(f.graph.textureDesc(f.gbuffer).usage == (rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled));
    CHECK(f.graph.textureDesc(f.depth).usage == rhi::TextureUsage::DepthStencilAttachment);
}

TEST_CASE("RenderGraph: depth read + sampling in one pass merges to DepthStencilReadOnly")
{
    DeferredFrame f;
    f.graph.addPass("SoftParticles", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.readDepth(f.depth);
        b.sample(f.depth, PS);
        b.writeColor(f.backbuffer, LoadOp::Load);
    });
    const CompileResult& r = f.graph.compile();
    REQUIRE(r.ok);
    CHECK(rhi::hasAll(f.graph.textureDesc(f.depth).usage,
                      rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled));
}

TEST_CASE("RenderGraph: sampling an attachment of the same pass is an error (feedback loop)")
{
    RenderGraph g;
    g.addPass("Bad", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.createTexture("T", tex2D(rhi::Format::RGBA8_UNORM));
        b.writeColor(t, LoadOp::Clear);
        b.sample(t, PS);
        b.setSideEffects();
    });
    const CompileResult& r = g.compile();
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.errors.empty());
}

TEST_CASE("RenderGraph: imported textures must allow the declared accesses")
{
    RenderGraph g;
    rhi::TextureDesc d = tex2D(rhi::Format::RGBA8_UNORM);
    d.usage = rhi::TextureUsage::ColorAttachment;  // not Sampled
    RGTexture t = g.importTexture("NotSampleable", d, {}, {});
    g.addPass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(t, PS);
        b.setSideEffects();
    });
    CHECK_FALSE(g.compile().ok);
}

TEST_CASE("RenderGraph: invalid transient descriptions are reported")
{
    RenderGraph g;
    g.addPass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.createTexture("Compressed", tex2D(rhi::Format::BC7_UNORM, 256, 256));
        b.writeColor(t, LoadOp::Clear);  // can't render to BC7
        b.setSideEffects();
    });
    CHECK_FALSE(g.compile().ok);
}

TEST_CASE("RenderGraph: invalid handles and wrong resource kinds are reported")
{
    RenderGraph g;
    RGBuffer buf;
    g.addPass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.sample(RGTexture{}, PS);
        rhi::BufferDesc d;
        d.size = 16;
        buf = b.createBuffer("B", d);
        b.sample(RGTexture{buf.id}, PS);  // a buffer id used as a texture
        b.setSideEffects();
    });
    const CompileResult& r = g.compile();
    CHECK_FALSE(r.ok);
    CHECK(r.errors.size() == 2);
}

TEST_CASE("RenderGraph: reading a never-written transient produces a warning")
{
    RenderGraph g;
    g.addPass("P", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        RGTexture t = b.createTexture("Garbage", tex2D(rhi::Format::RGBA8_UNORM));
        b.sample(t, PS);
        b.setSideEffects();
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
        bb = graph.importTexture("BB", backbufferDesc(), {}, kAcquired, kPresent);
        const rhi::TextureDesc d = tex2D(rhi::Format::RGBA16_FLOAT, 1024, 1024);
        graph.addPass("A", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            t1 = b.createTexture("T1", d);
            b.writeColor(t1, LoadOp::Clear);
        });
        graph.addPass("B", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            t2 = b.createTexture("T2", d);
            b.sample(t1, PS);
            b.writeColor(t2, LoadOp::Clear);
        });
        graph.addPass("C", rhi::QueueType::Compute, [&](PassBuilder& b) {
            t3 = b.createTexture("T3", d);
            rhi::BufferDesc bd;
            bd.size = 1u << 20;
            scratch = b.createBuffer("Scratch", bd);
            b.sample(t2, CS);
            b.writeStorage(scratch, CS, true);
            b.writeStorage(t3, CS, true);
        });
        graph.addPass("D", rhi::QueueType::Graphics, [&](PassBuilder& b) {
            b.sample(t3, PS);
            b.writeColor(bb, LoadOp::DontCare);
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
    CHECK(r.lifetimes[c.t1.id].firstPass == 0);
    CHECK(r.lifetimes[c.t1.id].lastPass == 1);
    CHECK(r.lifetimes[c.t3.id].firstPass == 2);

    CHECK(r.allocations[c.t3.id].slot == r.allocations[c.t1.id].slot);  // T3 reuses T1's memory
    CHECK(r.allocations[c.t3.id].aliasPredecessor == c.t1.id);
    CHECK(r.allocations[c.t2.id].slot != r.allocations[c.t1.id].slot);  // overlapping lifetimes
    CHECK(r.allocations[c.scratch.id].slot != r.allocations[c.t1.id].slot);  // buffers never alias textures
    CHECK(r.allocations[c.bb.id].slot == kInvalidResource);  // imported: not managed

    CHECK(r.transientBytesAliased < r.transientBytesUnaliased);
    CHECK(r.slotSizes.size() == 3);  // {T1/T3, T2, Scratch}
}

TEST_CASE("RenderGraph: first use of an aliased resource waits for the previous occupant")
{
    PostChain c;
    const CompileResult& r = c.graph.compile();
    REQUIRE(r.ok);
    const GraphBarrier* b = findBarrier(r.passes[2], c.t3.id);
    REQUIRE(b != nullptr);
    CHECK(b->before.layout == TextureLayout::Undefined);
    CHECK(rhi::hasAny(b->before.stages, PS));  // T1 was last sampled in the pixel shader (pass B)
}

TEST_CASE("RenderGraph: aliasing can be disabled, and custom size callbacks are honored")
{
    PostChain c;
    CompileOptions opts;
    opts.enableAliasing = false;
    opts.textureSize = [](const rhi::TextureDesc&) -> uint64_t { return 1000; };
    opts.bufferSize = [](const rhi::BufferDesc&) -> uint64_t { return 10; };
    const CompileResult& r = c.graph.compile(opts);
    REQUIRE(r.ok);
    CHECK(r.slotSizes.size() == 4);
    CHECK(r.transientBytesUnaliased == 3010);
    CHECK(r.transientBytesAliased == 3010);
}

// =====================================================================================================================
// Execution
// =====================================================================================================================

TEST_CASE("RenderGraph: execute runs surviving passes in order with resolved barriers")
{
    RenderGraph g;
    std::vector<std::string> log;
    const auto bbHandle = rhi::TextureHandle::fromParts(42, 3);
    RGTexture bb = g.importTexture("BB", backbufferDesc(), bbHandle, kAcquired, kPresent);
    RGTexture rt;
    rhi::TextureHandle seenTransient;

    g.addPass(
        "Draw", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
            rt = b.createTexture("RT", tex2D(rhi::Format::RGBA8_UNORM));
            b.writeColor(rt, LoadOp::Clear);
        },
        [&](PassContext& ctx) {
            log.push_back(std::string(ctx.passName()));
            seenTransient = ctx.texture(rt);
            CHECK(ctx.commandList() == nullptr);
        });
    g.addPass(
        "Unused", rhi::QueueType::Graphics,
        [&](PassBuilder& b) { b.writeColor(b.createTexture("X", tex2D(rhi::Format::RGBA8_UNORM)), LoadOp::Clear); },
        [&](PassContext&) { log.push_back("Unused"); });
    g.addPass(
        "Blit", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
            b.sample(rt, PS);
            b.writeColor(bb, LoadOp::DontCare);
        },
        [&](PassContext& ctx) {
            log.push_back(std::string(ctx.passName()));
            CHECK(ctx.texture(bb) == bbHandle);
            CHECK(ctx.textureDesc(bb).format == rhi::Format::BGRA8_SRGB);
        });

    REQUIRE(g.compile().ok);

    std::vector<rhi::TextureHandle> physical(g.resourceCount());
    const auto rtHandle = rhi::TextureHandle::fromParts(5, 1);
    physical[rt.id] = rtHandle;

    std::vector<rhi::TextureBarrier> submitted;
    ExecuteContext ctx;
    ctx.transientTextures = physical;
    ctx.submitBarriers = [&](std::span<const rhi::TextureBarrier> tex, std::span<const rhi::BufferBarrier> buf) {
        submitted.insert(submitted.end(), tex.begin(), tex.end());
        CHECK(buf.empty());
    };
    g.execute(ctx);

    REQUIRE(log.size() == 2);
    CHECK(log[0] == "Draw");
    CHECK(log[1] == "Blit");
    CHECK(seenTransient == rtHandle);

    // Draw: RT init. Blit: RT -> SRV, BB init. Final: BB -> Present.
    REQUIRE(submitted.size() == 4);
    CHECK(submitted[0].texture == rtHandle);
    CHECK(submitted.back().texture == bbHandle);
    CHECK(submitted.back().after.layout == TextureLayout::Present);
}

TEST_CASE("RenderGraph: graphviz output lists passes and marks culled ones")
{
    DeferredFrame f;
    f.graph.addPass("Orphan", rhi::QueueType::Graphics, [&](PassBuilder& b) {
        b.writeColor(b.createTexture("Lonely", tex2D(rhi::Format::RGBA8_UNORM)), LoadOp::Clear);
    });
    f.graph.compile();
    const std::string dot = f.graph.toGraphviz();
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
    CHECK(f.graph.passCount() == 0);
    CHECK(f.graph.resourceCount() == 0);
    const CompileResult& r = f.graph.compile();
    CHECK(r.ok);
    CHECK(r.passes.empty());
}
