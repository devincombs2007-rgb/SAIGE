// Imported model surfaces: base color texture and factor, emissive texture and factor, occlusion, alpha mask, the same
// hemispherical ambient plus sun and fog as the 0.1 cube and ground, so imported and procedural objects sit together.
// Occlusion scales the ambient term only, as glTF defines it. The metallic-roughness and normal textures and their factors
// are bound but not read yet (milestones M2 and M3 of docs/rendering/material-architecture.md).
//
// Bindings (RHI bind groups, docs/rendering/rhi-design.md section 4.2):
//   set 0, per material:  0..4 textures (base color, metallic-roughness, normal, occlusion, emissive),
//                         5..9 their samplers in the same order, 10 material uniform block
//   set 1, per renderer:  0 scene constants (fog, sun), immutable
// Push constants (128 bytes, the portable minimum): the camera's view-projection, the instance's world matrix as three rows
// of a 3x4 affine, and the camera position with the sign of the matrix's determinant. Normals use the cofactor matrix of the
// upper 3x3, which stays correct under non-uniform scale, shear and mirroring without a per-vertex inverse; the sign
// restores the outward direction for a mirrored instance. Which side is "front" for a mirrored instance is decided by the
// pipeline's front-face winding (the renderer picks a clockwise variant), so the pixel shader only needs SV_IsFrontFace.
//
// @shader   SceneModel
// @entry    VSMain vs
// @entry    PSMain ps

struct ModelPush {
    column_major float4x4 view_proj;
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
    float4 camera_sign;
};
[[vk::push_constant]] ConstantBuffer<ModelPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_base_color;          // sRGB
[[vk::binding(1, 0)]] Texture2D<float4> g_metallic_roughness;  // linear; G roughness, B metallic (not read yet)
[[vk::binding(2, 0)]] Texture2D<float4> g_normal;              // linear tangent-space (not read yet)
[[vk::binding(3, 0)]] Texture2D<float4> g_occlusion;           // linear; R
[[vk::binding(4, 0)]] Texture2D<float4> g_emissive;            // sRGB
[[vk::binding(5, 0)]] SamplerState g_base_color_sampler;
[[vk::binding(6, 0)]] SamplerState g_metallic_roughness_sampler;
[[vk::binding(7, 0)]] SamplerState g_normal_sampler;
[[vk::binding(8, 0)]] SamplerState g_occlusion_sampler;
[[vk::binding(9, 0)]] SamplerState g_emissive_sampler;
[[vk::binding(10, 0)]] cbuffer Material
{
    float4 mat_base_color;      // linear factor, multiplied with the texture
    float4 mat_emissive_cutoff; // emissive rgb (linear, may exceed 1), alpha cutoff in w (0 = opaque, no test)
    float4 mat_mr_normal_occ;   // metallic, roughness, normal scale, occlusion strength
};

[[vk::binding(0, 1)]] cbuffer SceneConstants
{
    float4 sc_fog_color;      // linear rgb; fog density per meter in w
    float4 sc_sun_direction;  // unit vector toward the sun
    float4 sc_sun_color;
    float4 sc_ambient_ground; // ambient light from below (+Z up is the sky color)
};

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_pos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
};

VSOutput VSMain(VSInput input)
{
    const float4 p = float4(input.position, 1.0);
    const float3 world = float3(dot(pc.model_row0, p), dot(pc.model_row1, p), dot(pc.model_row2, p));
    const float3 c0 = float3(pc.model_row0.x, pc.model_row1.x, pc.model_row2.x);
    const float3 c1 = float3(pc.model_row0.y, pc.model_row1.y, pc.model_row2.y);
    const float3 c2 = float3(pc.model_row0.z, pc.model_row1.z, pc.model_row2.z);
    const float3 n = input.normal.x * cross(c1, c2) + input.normal.y * cross(c2, c0) + input.normal.z * cross(c0, c1);

    VSOutput output;
    output.position = mul(pc.view_proj, float4(world, 1.0));
    output.world_pos = world;
    output.normal = n * pc.camera_sign.w;
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input, bool front : SV_IsFrontFace) : SV_Target0
{
    const float4 base = g_base_color.Sample(g_base_color_sampler, input.uv) * mat_base_color;
    if (mat_emissive_cutoff.w > 0.0 && base.a < mat_emissive_cutoff.w) {
        discard;
    }
    float3 n = normalize(input.normal);
    if (!front) {
        n = -n; // the back of a double-sided surface lights with the reversed normal
    }
    const float n_dot_l = saturate(dot(n, sc_sun_direction.xyz));
    const float occlusion = lerp(1.0, g_occlusion.Sample(g_occlusion_sampler, input.uv).r, mat_mr_normal_occ.w);
    const float3 ambient = lerp(sc_ambient_ground.rgb, sc_fog_color.rgb * 0.6, n.z * 0.5 + 0.5) * occlusion;
    const float3 emissive = g_emissive.Sample(g_emissive_sampler, input.uv).rgb * mat_emissive_cutoff.rgb;
    const float3 lit = base.rgb * (ambient + sc_sun_color.rgb * n_dot_l) + emissive;
    const float distance_m = length(input.world_pos - pc.camera_sign.xyz);
    const float fog = 1.0 - exp(-distance_m * sc_fog_color.w);
    return float4(lerp(lit, sc_fog_color.rgb, saturate(fog)), 1.0);
}
