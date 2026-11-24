// The sun shadow pass for alpha-masked (glTF MASK) imported materials: depth only like shadow_depth.hlsl, but with a pixel stage that
// discards what the visible pass discards. It reads the material's own set 0 bind group (the one the visible pass binds), so the texture,
// sampler, factor and cutoff are literally the visible pass's: a fragment is dropped when base color alpha (texture times factor) is below the
// cutoff, the same test as model.hlsl. Only materials with a cutoff use this variant; opaque ones keep the vertex-only shader.
//
// Set 0 (declared as in model.hlsl, only what this stage reads): 0 the base color texture, 5 its sampler, 10 the material block.
// Push constants (112 bytes): the light's view-projection and the instance's world matrix rows, as in shadow_depth.hlsl.
//
// @shader   SceneShadowDepthMask
// @entry    VSMain vs
// @entry    PSMain ps

struct ShadowPush {
    column_major float4x4 light_view_proj;
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
};
[[vk::push_constant]] ConstantBuffer<ShadowPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_base_color;  // sRGB
[[vk::binding(5, 0)]] SamplerState g_base_color_sampler;
[[vk::binding(10, 0)]] cbuffer Material
{
    float4 mat_base_color;      // linear factor, multiplied with the texture
    float4 mat_emissive_cutoff; // emissive rgb (unused here), alpha cutoff in w
    float4 mat_mr_normal_occ;
    float4 mat_flags;
};

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    const float4 p = float4(input.position, 1.0);
    const float3 world = float3(dot(pc.model_row0, p), dot(pc.model_row1, p), dot(pc.model_row2, p));
    VSOutput o;
    o.position = mul(pc.light_view_proj, float4(world, 1.0));
    o.uv = input.uv;
    return o;
}

void PSMain(VSOutput input)
{
    const float4 base = g_base_color.Sample(g_base_color_sampler, input.uv) * mat_base_color;
    if (base.a < mat_emissive_cutoff.w) {
        discard;
    }
}
