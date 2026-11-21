#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/barrier.hpp"

using namespace saige::rhi;

static_assert(is_write_access(Access::ColorAttachmentWrite));
static_assert(!is_write_access(Access::ShaderSampledRead | Access::UniformRead));
static_assert(write_access_only(Access::ShaderStorageRead | Access::ShaderStorageWrite) == Access::ShaderStorageWrite);

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

TEST_CASE("Flags: has_any / has_all")
{
    const PipelineStage s = PipelineStage::VertexShader | PipelineStage::PixelShader;
    CHECK(has_any(s, PipelineStage::PixelShader));
    CHECK(has_all(s, PipelineStage::VertexShader | PipelineStage::PixelShader));
    CHECK_FALSE(has_all(s, PipelineStage::VertexShader | PipelineStage::ComputeShader));
    CHECK_FALSE(has_any(s, PipelineStage::ComputeShader));
}
