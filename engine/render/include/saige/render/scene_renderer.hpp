// SAIGE Render — the 0.1 scene renderer: a procedural checkerboard ground plus one lit cube per instance.
//
// Backend-agnostic: records into any rhi::CommandList and derives barriers with the RenderGraph. The frame
// sink adapter (engine/render_sink) feeds it from Core's FrameView.
#pragma once

#include "saige/render/camera.hpp"
#include "saige/render/gpu_resources.hpp"
#include "saige/render/model.hpp"
#include "saige/render/model_renderer.hpp"
#include "saige/render/math.hpp"
#include "saige/render/render_graph.hpp"
#include "saige/render/shadow_cascades.hpp"
#include "saige/render/tonemap.hpp"
#include "saige/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace saige::render {

/// Push-constant block shared with shaders/scene/scene_common.hlsli (keep in sync; layout checked below).
struct DrawConstants {
    Mat4 view_proj;
    float position_yaw[4];  ///< cube: base position + yaw; ground: center + half size
    float color_scale[4];   ///< cube: linear albedo + edge length
    float camera_fog[4];    ///< camera position + fog density
    float fog_color[4];     ///< linear fog/sky color
};
static_assert(sizeof(DrawConstants) == 128, "DrawConstants must fill exactly the guaranteed 128-byte push range");
static_assert(offsetof(DrawConstants, position_yaw) == 64);
static_assert(offsetof(DrawConstants, color_scale) == 80);
static_assert(offsetof(DrawConstants, camera_fog) == 96);
static_assert(offsetof(DrawConstants, fog_color) == 112);

struct CubeVertex {
    Float3 position;
    Float3 normal;
};
static_assert(sizeof(CubeVertex) == 24);

/// Unit cube centered on the origin: 24 vertices (flat normals), 36 indices, counter-clockwise from outside.
struct CubeMesh {
    CubeVertex vertices[24];
    uint16_t indices[36];
};
[[nodiscard]] CubeMesh make_unit_cube();

enum class CameraMode : uint8_t {
    Follow,  ///< Tracks the first instance from a fixed offset (desktop default).
    Fixed,   ///< Fixed eye and target (tests and captures).
};

/// Sun shadows for imported models. Off by default: with `enabled = false` the renderer uses the shadow-free model shader and acquires
/// nothing for shadows (no map, sampler, pass or per-frame buffers; no sampled-depth or comparison-sampler requirement), so every image is
/// exactly what it was before they existed. One directional shadow map fitted to the camera's view range [near, distance]: a bounding
/// sphere around that slice, snapped to whole texels, so shadow edges do not shimmer when the camera moves. The light direction is
/// `RendererSettings::lighting.sun_direction`, the same normalized vector the lighting uses. Opaque casters only: authored MASK and
/// BLEND materials do not cast (see docs/rendering/shadows.md).
struct SunShadowSettings {
    bool enabled = false;
    uint32_t map_size = 2048;              ///< texels per side, 16 to 8192
    float distance = 60.0f;                ///< meters of view depth that receive shadows; beyond it the sun is unoccluded
    float caster_margin = 50.0f;           ///< how far toward the sun the light's near plane is pulled, so off-screen casters still shadow
    float depth_bias_constant = -1024.0f;  ///< rasterizer depth bias of the shadow pass (reverse-Z: negative moves casters away from the sun)
    float depth_bias_slope = -1.5f;
    float normal_offset_texels = 1.5f;     ///< receivers move this many shadow texels along their geometric normal before the lookup
    uint32_t filter_radius = 1;            ///< percentage-closer filter radius in texels: (2r + 1)^2 taps; 0 = a single tap
};

struct RendererSettings {
    CameraMode camera_mode = CameraMode::Follow;
    Float3 follow_offset{-6.0f, -4.5f, 3.5f};  ///< behind (-X), to the right (-Y) and above the target
    Float3 fixed_eye{-6.0f, -4.5f, 4.0f};
    Float3 fixed_target{0.0f, 0.0f, 0.5f};
    float fov_y_radians = 60.0f * pi / 180.0f;
    float near_plane = 0.1f;
    rhi::ClearColor sky{0.30f, 0.50f, 0.82f, 1.0f};  ///< linear; also the fog color
    float fog_density = 0.012f;                      ///< per meter
    ModelShading model_shading = ModelShading::Legacy;  ///< shading model of imported models (the 0.1 cube and ground are unaffected)
    SceneLighting lighting;                          ///< sun and ground ambient of imported models; defaults are the long-standing look
    SunShadowSettings shadows;                       ///< sun shadows for imported models; off by default
    /// Composite glTF BLEND materials with sorted alpha blending. Off by default: BLEND draws opaque exactly as in 0.2.
    bool alpha_blending = false;
    float ground_half_size = 150.0f;                 ///< meters, centered under the camera target
    float cube_edge = 1.0f;                          ///< meters
    float spin_radians_per_second = 0.8f;
    double seconds_per_tick = 1.0 / 60.0;           ///< matches Core's default fixed step
    /// HDR output: the scene is drawn in linear light into an RGBA16_FLOAT target and a tone-map pass writes the output target
    /// (docs/rendering/material-architecture.md section 4). Off by default: the 0.2 path draws straight into the output target.
    bool hdr = false;
    ToneMapSettings tone_map;                       ///< used when `hdr` is set
};

struct SceneInstance {
    Float3 position;  ///< base of the cube (world meters)
    Float3 color{0.85f, 0.30f, 0.10f};  ///< linear albedo
    float yaw_offset = 0.0f;
    float scale = 1.0f;  ///< uniform; multiplies RendererSettings::cube_edge. Full TRS arrives with the instance buffer.
};

struct SceneFrame {
    uint64_t tick = 0;
    double alpha = 0.0;
    std::span<const SceneInstance> instances;
    /// When set, replaces the settings-driven camera (follow/fixed) for this frame. Not retained after render().
    const Camera* camera = nullptr;
    /// Imported models to draw this frame (not retained after render()); they share the depth buffer with everything else.
    std::span<const ModelDraw> models;
};

/// Where a frame is rendered. The renderer leaves `texture` in `final_state` (or its last-use state).
struct RenderTarget {
    rhi::TextureHandle texture;
    uint32_t width = 0;
    uint32_t height = 0;
    rhi::ResourceState initial_state;
    std::optional<rhi::ResourceState> final_state;
    /// Optional: copy the finished image into this GpuToCpu buffer (tight rows) after rendering.
    rhi::BufferHandle readback_buffer;
};

/// Timestamp slots the renderer writes (rhi::CommandList::write_timestamp); the span between them is the scene pass.
inline constexpr uint32_t timestamp_scene_begin = 0;
inline constexpr uint32_t timestamp_scene_end = 1;

struct FrameStats {
    uint32_t draw_calls = 0;
    uint32_t barriers = 0;
    uint32_t model_instances = 0;          ///< imported-model placements drawn
    uint32_t model_instances_skipped = 0;  ///< non-finite or degenerate world matrices
    uint32_t model_blended_primitives = 0; ///< imported-model primitives drawn in the sorted alpha-blended phase
    uint32_t shadow_draw_calls = 0;        ///< draws of the sun shadow pass (also counted in draw_calls)
};

class SceneRenderer {
public:
    /// Creates pipelines and the cube mesh for targets of `color_format`. Throws on device failure.
    SceneRenderer(rhi::Device& device, rhi::Format color_format, RendererSettings settings = {});
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Records the frame into `cmd`. A zero-sized target records nothing.
    FrameStats render(rhi::CommandList& cmd, const SceneFrame& frame, const RenderTarget& target);

    [[nodiscard]] Mat4 view_projection(const SceneFrame& frame, float aspect) const;
    [[nodiscard]] Float3 camera_eye(const SceneFrame& frame) const;
    [[nodiscard]] float instance_yaw(const SceneFrame& frame, const SceneInstance& instance) const;
    [[nodiscard]] const RendererSettings& settings() const noexcept { return settings_; }
    /// Owns the GPU resources of every staged model; counters and the leak report live here.
    [[nodiscard]] GpuResourceRegistry& registry() noexcept { return registry_; }
    /// The imported-model renderer, created on first use (the 0.1 cube path never pays for it).
    [[nodiscard]] ModelRenderer& model_renderer();
    /// The output target's format (what `render` writes last).
    [[nodiscard]] rhi::Format color_format() const noexcept { return color_format_; }
    /// The format the scene itself is drawn in: the output format, or RGBA16_FLOAT when `settings().hdr` is set.
    [[nodiscard]] rhi::Format scene_format() const noexcept { return scene_format_; }

private:
    void ensure_targets(uint32_t width, uint32_t height);
    [[nodiscard]] Float3 camera_target(const SceneFrame& frame) const;

    rhi::Device& device_;
    rhi::Format color_format_;
    rhi::Format scene_format_;
    RendererSettings settings_;
    rhi::ShaderHandle cube_vs_, cube_ps_, ground_vs_, ground_ps_;
    rhi::PipelineHandle cube_pipeline_, ground_pipeline_;
    rhi::BufferHandle cube_vertices_, cube_indices_;
    rhi::TextureHandle depth_;
    uint32_t target_width_ = 0;
    uint32_t target_height_ = 0;
    rhi::ResourceState depth_state_;
    std::unique_ptr<ToneMapper> tone_mapper_;  ///< HDR only
    rhi::TextureHandle hdr_;                   ///< HDR only: the scene's linear float target, sized like the output
    rhi::BindGroupHandle hdr_input_;           ///< the tone mapper's view of hdr_
    rhi::ResourceState hdr_state_;
    rhi::ResourceState shadow_state_;  ///< the sun shadow map's state after the last frame (undefined until its first use)
    bool shadow_ready_ = false;        ///< the shadow map has been taken out of the undefined layout
    RenderGraph graph_;
    GpuResourceRegistry registry_;            // declared before models_: the model renderer's resources go first
    std::unique_ptr<ModelRenderer> models_;
};

} // namespace saige::render
