// Imported model surfaces: base color texture and factor, emissive texture and factor, occlusion, alpha mask, the same
// hemispherical ambient plus sun and fog as the 0.1 cube and ground, so imported and procedural objects sit together.
// Occlusion scales the ambient term only, as glTF defines it. The metallic-roughness texture and tangent-space normal
// mapping apply in mode 2 only (docs/rendering/material-architecture.md); mode 0 stays the 0.2 look.
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
// Renderer modes: 0 preserves SAIGE 0.2, 1 adds emissive/occlusion textures to the same lighting,
// and 2 uses metallic-roughness shading (pbr.hlsli).
//
// Sun shadows are a compile-time permutation. This file, as is (SAIGE_SUN_SHADOWS = 0), is the 0.2/0.3 shader with no shadow bindings, so a
// renderer that does not ask for shadows needs nothing new from the device. model_shadowed.hlsl defines SAIGE_SUN_SHADOWS = 1 and includes
// this file: set 1 gains 3 the sun shadow map (a depth texture), 4 its comparison sampler, 5 the shadow block for the frame (the light's
// view-projection and filter parameters), and the sun term is multiplied by `sun_visibility`.
//
// @shader   SceneModel
// @entry    VSMain vs
// @entry    PSMain ps

#include "pbr.hlsli"

#ifndef SAIGE_SUN_SHADOWS
#define SAIGE_SUN_SHADOWS 0
#endif
#ifndef SAIGE_IBL
#define SAIGE_IBL 0
#endif

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
#if SAIGE_IBL
// Image-based lighting (metallic-roughness shading only; model_ibl.hlsl): the prefiltered environment as one equirect texture whose mip i is the
// environment at perceptual roughness i / (levels - 1), and nine SH coefficients for the diffuse term. docs/rendering/ibl-environment.md.
[[vk::binding(6, 1)]] Texture2D<float4> g_ibl_env;
[[vk::binding(7, 1)]] SamplerState g_ibl_sampler;  // linear, linear mips, wrap in u, clamp in v
[[vk::binding(8, 1)]] cbuffer IblConstants
{
    float4 ibl_sh[9];   // radiance coefficients, order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22
    float4 ibl_params;  // x: levels - 1, y: intensity
};

/// The diffuse radiance factor (irradiance / pi) for unit normal n: the SH coefficients scaled by the clamped cosine's band weights 1, 2/3, 1/4.
float3 ibl_diffuse(float3 n)
{
    const float x = n.x, y = n.y, z = n.z;
    float3 sum = ibl_sh[0].rgb * 0.282094791773878;
    sum += (ibl_sh[1].rgb * (0.488602511902920 * y) + ibl_sh[2].rgb * (0.488602511902920 * z) + ibl_sh[3].rgb * (0.488602511902920 * x)) * (2.0 / 3.0);
    sum += (ibl_sh[4].rgb * (1.092548430592079 * x * y) + ibl_sh[5].rgb * (1.092548430592079 * y * z) + ibl_sh[6].rgb * (0.315391565252520 * (3.0 * z * z - 1.0)) +
            ibl_sh[7].rgb * (1.092548430592079 * x * z) + ibl_sh[8].rgb * (0.546274215296040 * (x * x - y * y))) * 0.25;
    return max(sum, float3(0.0, 0.0, 0.0)) * ibl_params.y;
}

/// The prefiltered environment toward direction r at perceptual roughness (explicit LOD, so it is safe in any control flow). u = atan2(y, x) / 2 pi + 0.5 and
/// v = acos(z) / pi: +X is the middle of the image and +Z the top row.
float3 ibl_specular(float3 r, float roughness)
{
    const float2 uv = float2(atan2(r.y, r.x) * 0.15915494309189535 + 0.5, acos(clamp(r.z, -1.0, 1.0)) * 0.3183098861837907);
    return g_ibl_env.SampleLevel(g_ibl_sampler, uv, saturate(roughness) * ibl_params.x).rgb * ibl_params.y;
}
#endif
#if SAIGE_SUN_SHADOWS
[[vk::binding(3, 1)]] Texture2D<float> g_shadow_map;            // sun depth, reverse-Z (1 nearest the light)
[[vk::binding(4, 1)]] SamplerComparisonState g_shadow_sampler;  // nearest filtering, GreaterEqual
[[vk::binding(5, 1)]] cbuffer ShadowConstants
{
    column_major float4x4 sh_light_view_proj;
    float4 sh_params; // x: shadow-map texel size in meters, y: normal offset in texels, z: filter radius in texels (0 = one tap), w: 1 when shadows are on
    float4 sh_map;    // x: 1 / shadow-map size in texels
};

/// The fraction of the sun that reaches `world_pos` (1 = fully lit, 0 = shadowed): a percentage-closer filter over (2r+1)^2 comparison
/// taps. The position is first moved along the geometric normal by a fixed number of shadow texels, which removes the self-shadowing
/// that a depth bias alone cannot (grazing surfaces). Outside the map, or beyond its depth range, the sun is not occluded. Explicit-LOD
/// sampling only, so it is safe in any control flow.
float sun_visibility(float3 world_pos, float3 geometric_normal)
{
    if (sh_params.w < 0.5) {
        return 1.0;
    }
    const float3 offset_pos = world_pos + geometric_normal * (sh_params.y * sh_params.x);
    const float4 clip = mul(sh_light_view_proj, float4(offset_pos, 1.0));
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);  // texture v = 0 is the top row; NDC +Y is up
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    const int radius = (int)sh_params.z;
    float sum = 0.0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            sum += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv + float2(x, y) * sh_map.x, ndc.z);
        }
    }
    const float taps = (float)(2 * radius + 1);
    return sum / (taps * taps);
}
#endif

float3 ambient_radiance(float3 direction)
{
    return lerp(sc_ambient_ground.rgb, sc_fog_color.rgb * 0.6, direction.z * 0.5 + 0.5);
}

/// The material contract made concrete at one pixel: factors times textures, in the color space each slot is stored in. `n`, `t`
/// and `b` are the geometric frame facing the viewer; with `pbr` and a nonzero `t`, the tangent-space normal texture (x and y
/// scaled by the material's normal scale) replaces `n`.
///
/// Every slot is read with SampleGrad and the caller's screen-space UV derivatives, taken at the top of the pixel shader before
/// the alpha-mask discard and before the per-fragment tangent-frame guard. Implicit-LOD Sample here would take its derivatives
/// from quad neighbours that may already have been discarded, or that took the other side of the tangent-validity branch, and
/// Vulkan leaves derivatives undefined in non-uniform control flow. (`pbr` and `mat_flags` are uniform: constant buffers.)
SurfaceData evaluate_surface(float2 uv, float2 uv_dx, float2 uv_dy, float4 base, float3 n, float3 t, float3 b, bool pbr)
{
    float3 normal = n;
    if (pbr && mat_flags.x > 0.5) {
        float3 ts = g_normal.SampleGrad(g_normal_sampler, uv, uv_dx, uv_dy).xyz * 2.0 - 1.0;
        if (dot(t, t) > 1e-12) {
            ts.xy *= mat_mr_normal_occ.z;
            normal = normalize(t * ts.x + b * ts.y + n * ts.z);
        }
    }
    SurfaceData s;
    const float4 mr = pbr ? g_metallic_roughness.SampleGrad(g_metallic_roughness_sampler, uv, uv_dx, uv_dy) : 1.0;
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.roughness = mat_mr_normal_occ.y * mr.g;
    s.metallic = mat_mr_normal_occ.x * mr.b;
    s.occlusion = lerp(1.0, g_occlusion.SampleGrad(g_occlusion_sampler, uv, uv_dx, uv_dy).r, mat_mr_normal_occ.w);
    s.normal = normal;
    s.emissive = g_emissive.SampleGrad(g_emissive_sampler, uv, uv_dx, uv_dy).rgb * mat_emissive_cutoff.rgb;
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
    // Screen-space UV derivatives first, in uniform control flow, before the discard and the tangent-frame branches below.
    const float2 uv_dx = ddx(input.uv);
    const float2 uv_dy = ddy(input.uv);
    float3 n = normalize(input.normal);
    float3 t = float3(0.0, 0.0, 0.0);
    float3 b = float3(0.0, 0.0, 0.0);
    // Orthonormalize the interpolated tangent against the interpolated normal; the handedness is whatever the transformed frame
    // says. The projected tangent can vanish (a tangent parallel to the normal, or interpolation cancelling the two), and
    // normalizing it would produce NaN, so the frame is used only when the projection keeps a meaningful part of the tangent;
    // otherwise t and b stay zero and the surface keeps its geometric normal.
    const float tangent_len2 = dot(input.tangent, input.tangent);
    if (tangent_len2 > 1e-12) {
        const float3 projected = input.tangent - n * dot(n, input.tangent);
        const float projected_len2 = dot(projected, projected);
        if (projected_len2 > 1e-6 * tangent_len2) {
            t = projected * rsqrt(projected_len2);
            const float3 cross_nt = cross(n, t);
            b = cross_nt * (dot(cross_nt, input.bitangent) < 0.0 ? -1.0 : 1.0);
        }
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
#if SAIGE_SUN_SHADOWS
    const float sun_vis = sun_visibility(input.world_pos, n);
    const float n_dot_l = saturate(dot(n, sc_sun_direction.xyz)) * sun_vis;  // the sun term of the two non-PBR modes, shadowed
#define SUN_RADIANCE (sc_sun_color.rgb * sun_vis)
#else
    const float n_dot_l = saturate(dot(n, sc_sun_direction.xyz));
#define SUN_RADIANCE sc_sun_color.rgb
#endif
    float3 lit;
    if (sc_flags.x < 0.5) {
        // Legacy, the 0.2 look exactly: base color texture and factor, the emissive *factor*, hemispherical ambient plus sun.
        lit = base.rgb * (ambient_radiance(n) + sc_sun_color.rgb * n_dot_l) + mat_emissive_cutoff.rgb;
    } else {
        const bool pbr = sc_flags.x >= 1.5;
        const SurfaceData surface = evaluate_surface(input.uv, uv_dx, uv_dy, base, n, t, b, pbr);
        if (pbr) {
            const float3 mapped = surface.normal;  // the normal-mapped (or geometric) normal
            const float3 v = to_camera / max(distance_m, 1e-6);
            const float3 r = reflect(-v, mapped);
            const float2 table_uv = float2(saturate(dot(mapped, v)), clamp(surface.roughness, MIN_ROUGHNESS, 1.0));
            const float2 env_brdf = g_env_brdf.SampleLevel(g_env_brdf_sampler, table_uv, 0.0);
#if SAIGE_IBL
            lit = shade_directional(surface, v, sc_sun_direction.xyz, SUN_RADIANCE) +
                  shade_ambient(surface, env_brdf, ibl_diffuse(mapped), ibl_specular(r, clamp(surface.roughness, MIN_ROUGHNESS, 1.0))) + surface.emissive;
#else
            lit = shade_directional(surface, v, sc_sun_direction.xyz, SUN_RADIANCE) +
                  shade_ambient(surface, env_brdf, ambient_radiance(mapped), ambient_radiance(r)) + surface.emissive;
#endif
        } else {
            lit = base.rgb * (ambient_radiance(n) * surface.occlusion + sc_sun_color.rgb * n_dot_l) + surface.emissive;
        }
    }
    const float fog = 1.0 - exp(-distance_m * sc_fog_color.w);
    // A blended material outputs its straight (non-premultiplied) alpha for the pipeline's source-over blend; everything else
    // writes 1 as before. Fog mixes the color only, so a faded surface still fades into the fogged scene behind it.
    const float out_alpha = mat_flags.y > 0.5 ? saturate(base.a) : 1.0;  // base.a is the surface alpha in every mode
    return float4(lerp(lit, sc_fog_color.rgb, saturate(fog)), out_alpha);
}
