#include "saige/gfx_test/saige_test.h"

#include "saige/rhi/barrier.h"

using namespace saige::rhi;

static_assert(isWriteAccess(Access::ColorAttachmentWrite));
static_assert(!isWriteAccess(Access::ShaderSampledRead | Access::UniformRead));
static_assert(writeAccessOnly(Access::ShaderStorageRead | Access::ShaderStorageWrite) == Access::ShaderStorageWrite);

TEST_CASE("ResourceState: write detection and equality")
{
    const ResourceState rt{PipelineStage::ColorAttachmentOutput,
                           Access::ColorAttachmentRead | Access::ColorAttachmentWrite, TextureLayout::ColorAttachment};
    const ResourceState srv{PipelineStage::PixelShader, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly};
    CHECK(rt.writes());
    CHECK_FALSE(srv.writes());
    CHECK(rt != srv);
    CHECK(srv == ResourceState{PipelineStage::PixelShader, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly});
}

TEST_CASE("Flags: hasAny / hasAll")
{
    const PipelineStage s = PipelineStage::VertexShader | PipelineStage::PixelShader;
    CHECK(hasAny(s, PipelineStage::PixelShader));
    CHECK(hasAll(s, PipelineStage::VertexShader | PipelineStage::PixelShader));
    CHECK_FALSE(hasAll(s, PipelineStage::VertexShader | PipelineStage::ComputeShader));
    CHECK_FALSE(hasAny(s, PipelineStage::ComputeShader));
}
