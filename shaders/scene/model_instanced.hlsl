// The instanced vertex stage of the model shaders (rigid, opaque primitives). It is model.hlsl's rigid vertex stage with one change: the instance's world matrix is not a push
// constant but row three-of-three of the instance buffer at set 2, binding 0 (a storage buffer of float4 rows, three per instance, indexed by SV_InstanceID, which in Vulkan includes
// the draw's first instance), so one draw call places many copies of a primitive. Everything else (the outputs, the cofactor normal, the mirrored sign in the push constants'
// camera_sign.w, the push layout) is the same as model.hlsl, so the pixel stage of any of the four model permutations (sun shadows and image-based lighting) pairs with this vertex
// stage unchanged. It is a separate source so that model.hlsl, and with it every existing generated header, stays byte for byte as it was.
//
// Bindings: set 0 per material, set 1 per renderer (the pixel stage's), set 2 binding 0 the instance rows. Push constants (128 bytes, the same struct as model.hlsl): view_proj,
// three model rows that this stage ignores, and the camera position with the determinant sign of the instance (instances of one draw share the sign: mirrored ones are drawn separately).
//
// @shader   SceneModelInstanced
// @entry    VSMain vs

struct ModelPush {
    column_major float4x4 view_proj;
    float4 model_row0;  // ignored here: the world matrix comes from the instance buffer
    float4 model_row1;
    float4 model_row2;
    float4 camera_sign;
};
[[vk::push_constant]] ConstantBuffer<ModelPush> pc;

[[vk::binding(0, 2)]] StructuredBuffer<float4> g_instance_rows;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;  // xyz: direction of increasing u, w: handedness (0 = none)
    uint instance : SV_InstanceID;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_pos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float3 tangent : TEXCOORD3;
    [[vk::location(4)]] float3 bitangent : TEXCOORD4;
};

VSOutput VSMain(VSInput input)
{
    const float4 row0 = g_instance_rows[input.instance * 3u + 0u];
    const float4 row1 = g_instance_rows[input.instance * 3u + 1u];
    const float4 row2 = g_instance_rows[input.instance * 3u + 2u];
    const float4 p = float4(input.position, 1.0);
    const float3 world = float3(dot(row0, p), dot(row1, p), dot(row2, p));
    const float3 c0 = float3(row0.x, row1.x, row2.x);
    const float3 c1 = float3(row0.y, row1.y, row2.y);
    const float3 c2 = float3(row0.z, row1.z, row2.z);
    const float3 n = input.normal.x * cross(c1, c2) + input.normal.y * cross(c2, c0) + input.normal.z * cross(c0, c1);

    VSOutput output;
    output.position = mul(pc.view_proj, float4(world, 1.0));
    output.world_pos = world;
    output.normal = n * pc.camera_sign.w;
    output.uv = input.uv;
    const float3 t_obj = input.tangent.xyz;
    const float3 b_obj = cross(input.normal, t_obj) * input.tangent.w;
    output.tangent = float3(dot(row0.xyz, t_obj), dot(row1.xyz, t_obj), dot(row2.xyz, t_obj));
    output.bitangent = float3(dot(row0.xyz, b_obj), dot(row1.xyz, b_obj), dot(row2.xyz, b_obj));
    return output;
}
