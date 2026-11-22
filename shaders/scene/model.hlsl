// Imported model surfaces: base color texture and factor, emissive texture and factor, occlusion, alpha mask, the same
// hemispherical ambient plus sun and fog as the 0.1 cube and ground, so imported and procedural objects sit together.
// Occlusion scales the ambient term only, as glTF defines it. The metallic-roughness texture and tangent-space normal
// mapping apply in mode 1 only (docs/rendering/material-architecture.md); mode 0 stays the 0.2 look.
//
// Bindings (RHI bind groups, docs/rendering/rhi-design.md section 4.2):
//   set 0, per material:  0..4 textures (base color, metallic-roughness, normal, occlusion, emissive),
//                         5..9 their samplers in the same order, 10 material uniform block
//   set 1, per renderer:  0 scene constants (fog, sun, shading mode), immutable; 1 environment BRDF table, 2 its sampler
// Push constants (128 bytes, the portable minimum): the camera's view-projection, the instance's world matrix as three rows
// of a 3x4 affine, and the camera position with the sign of the matrix's determinant. Normals use the cofactor matrix of the
// upper 3x3, which stays correct under non-uniform scale, shear and mirroring without a per-vertex inverse; the sign
// restores the outward direction for a mirrored instance. Which side is "front" for a mirrored instance is decided by the
// pipeline's front-face winding (the renderer picks a clockwise variant), so the pixel shader only needs SV_IsFrontFace.
//
// Two shading modes, chosen per renderer (SceneConstants.sc_flags.x): 0 is the 0.2 look (albedo times ambient plus sun);
// 1 is metallic-roughness physically based shading (pbr.hlsli), still written to the display-referred target until HDR (M4).
//
// @shader   SceneModel
// @entry    VSMain vs
// @entry    PSMain ps

#include "pbr.hlsli"

struct ModelPush {
    column_major float4x4 view_proj;
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
    float4 camera_sign;
};
[[vk::push_constant]] ConstantBuffer<ModelPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_base_color;          // sRGB
[[vk::binding(1, 0)]] Texture2D<float4> g_metallic_roughness;  // linear; G roughness, B metallic
[[vk::binding(2, 0)]] Texture2D<float4> g_normal;              // linear tangent-space, +Y up in the image (glTF)
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
    float4 mat_flags;           // x: 1 when the material has a normal texture; y: 1 when the material is alpha blended
};

[[vk::binding(0, 1)]] cbuffer SceneConstants
{
    float4 sc_fog_color;      // linear rgb; fog density per meter in w
    float4 sc_sun_direction;  // unit vector toward the sun
    float4 sc_sun_color;
    float4 sc_ambient_ground; // ambient light from below (+Z up is the sky color)
    float4 sc_flags;          // x: shading mode (0 legacy, 1 extended, 2 metallic-roughness)
};
[[vk::binding(1, 1)]] Texture2D<float2> g_env_brdf; // split-sum table: x = n.v, y = perceptual roughness -> (scale, bias)
[[vk::binding(2, 1)]] SamplerState g_env_brdf_sampler;

float3 ambient_radiance(float3 direction)
{
    return lerp(sc_ambient_ground.rgb, sc_fog_color.rgb * 0.6, direction.z * 0.5 + 0.5);
}

/// The material contract made concrete at one pixel: factors times textures, in the color space each slot is stored in. `n`,
/// `t` and `b` are the geometric frame facing the viewer; with `normal_mapping` and a nonzero `t`, the tangent-space normal
/// texture (x and y scaled by the material's normal scale) replaces `n`.
SurfaceData evaluate_surface(float2 uv, float4 base, float3 n, float3 t, float3 b, bool physically_based)
{
    float3 normal = n;
    if (physically_based && mat_flags.x > 0.5) {
        // Keep implicit derivatives in uniform flow; tangent validity can vary across a fragment quad.
        float3 ts = g_normal.Sample(g_normal_sampler, uv).xyz * 2.0 - 1.0;
        if (dot(t, t) > 1e-12) {
            ts.xy *= mat_mr_normal_occ.z;
            normal = normalize(t * ts.x + b * ts.y + n * ts.z);
        }
    }
    SurfaceData s;
    const float4 mr = physically_based ? g_metallic_roughness.Sample(g_metallic_roughness_sampler, uv) : 1.0;
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.roughness = mat_mr_normal_occ.y * mr.g;
    s.metallic = mat_mr_normal_occ.x * mr.b;
    s.occlusion = lerp(1.0, g_occlusion.Sample(g_occlusion_sampler, uv).r, mat_mr_normal_occ.w);
    s.normal = normal;
    s.emissive = g_emissive.Sample(g_emissive_sampler, uv).rgb * mat_emissive_cutoff.rgb;
    return s;
}

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT; // xyz: direction of increasing u, w: handedness (0 = none)
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_pos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float3 tangent : TEXCOORD3;   // world space, zero when the vertex has none
    [[vk::location(4)]] float3 bitangent : TEXCOORD4; // world space, the transformed object-space bitangent
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
    // Surface directions transform with the model matrix itself (only normals need the cofactor). The bitangent is built in object
    // space from the handedness and then transformed too, so a mirrored instance keeps a consistent, left-handed frame.
    const float3 t_obj = input.tangent.xyz;
    const float3 b_obj = cross(input.normal, t_obj) * input.tangent.w;
    output.tangent = float3(dot(pc.model_row0.xyz, t_obj), dot(pc.model_row1.xyz, t_obj), dot(pc.model_row2.xyz, t_obj));
    output.bitangent = float3(dot(pc.model_row0.xyz, b_obj), dot(pc.model_row1.xyz, b_obj), dot(pc.model_row2.xyz, b_obj));
    return output;
}

float4 PSMain(VSOutput input, bool front : SV_IsFrontFace) : SV_Target0
{
    float3 n = normalize(input.normal);
    float3 t = float3(0.0, 0.0, 0.0);
    float3 b = float3(0.0, 0.0, 0.0);
    const float tangent_len2 = dot(input.tangent, input.tangent);
    const float3 projected = input.tangent - n * dot(n, input.tangent);
    const float projected_len2 = dot(projected, projected);
    if (tangent_len2 > 1e-12 && projected_len2 > 1e-6 * tangent_len2) {
        t = projected * rsqrt(projected_len2);
        const float3 cross_nt = cross(n, t);
        b = cross_nt * (dot(cross_nt, input.bitangent) < 0.0 ? -1.0 : 1.0);
    }
    if (!front) {
        n = -n; // the back of a double-sided surface lights with the reversed frame (as the glTF reference viewer does)
        t = -t;
        b = -b;
    }
    const float4 base = g_base_color.Sample(g_base_color_sampler, input.uv) * mat_base_color;
    if (mat_emissive_cutoff.w > 0.0 && base.a < mat_emissive_cutoff.w) {
        discard;
    }
    const float3 to_camera = pc.camera_sign.xyz - input.world_pos;
    const float distance_m = length(to_camera);
    const float n_dot_l = saturate(dot(n, sc_sun_direction.xyz));
    float3 lit;
    if (sc_flags.x < 0.5) {
        lit = base.rgb * (ambient_radiance(n) + sc_sun_color.rgb * n_dot_l) + mat_emissive_cutoff.rgb;
    } else {
        const bool pbr = sc_flags.x >= 1.5;
        const SurfaceData surface = evaluate_surface(input.uv, base, n, t, b, pbr);
        n = surface.normal;
        if (pbr) {
            const float3 v = to_camera / max(distance_m, 1e-6);
            const float3 r = reflect(-v, n);
            const float2 table_uv = float2(saturate(dot(n, v)), clamp(surface.roughness, MIN_ROUGHNESS, 1.0));
            const float2 env_brdf = g_env_brdf.SampleLevel(g_env_brdf_sampler, table_uv, 0.0);
            lit = shade_directional(surface, v, sc_sun_direction.xyz, sc_sun_color.rgb) +
                  shade_ambient(surface, env_brdf, ambient_radiance(n), ambient_radiance(r)) + surface.emissive;
        } else {
            lit = base.rgb * (ambient_radiance(n) * surface.occlusion + sc_sun_color.rgb * n_dot_l) + surface.emissive;
        }
    }
    const float fog = 1.0 - exp(-distance_m * sc_fog_color.w);
    // A blended material outputs its straight (non-premultiplied) alpha for the pipeline's source-over blend; everything else
    // writes 1 as before. Fog mixes the color only, so a faded surface still fades into the fogged scene behind it.
    const float out_alpha = mat_flags.y > 0.5 ? saturate(base.a) : 1.0;
    return float4(lerp(lit, sc_fog_color.rgb, saturate(fog)), out_alpha);
}
