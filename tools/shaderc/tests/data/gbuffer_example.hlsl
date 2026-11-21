// GBuffer example: test fixture for saige-shaderc (not a production shader).
//
// @shader   GBufferExample
// @entry    VSMain vs
// @entry    PSMain ps
// @option   ALPHA_TEST     {0, 1}
// @option   NORMAL_MAP     {0, 1}
// @option   VERTEX_COLORS  {0, 1}
// @exclude  ALPHA_TEST && VERTEX_COLORS
// @budget   8

#include "gbuffer_common.hlsli"
#include <common/bindless.hlsli>

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    return float4(0, 0, 0, 1);
}

float4 PSMain() : SV_Target0
{
    return float4(1, 0, 1, 1);
}
