// SAIGE Render — the 0.1 scene renderer: a procedural checkerboard ground plus one lit cube per instance.
//
// Backend-agnostic: records into any rhi::CommandList and derives barriers with the RenderGraph. The frame
// sink adapter (engine/render_sink) feeds it from Core's FrameView.
#pragma once

#include "saige/render/atmosphere.hpp"
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
    uint32_t map_size = 2048;              ///< texels per side of each cascade, 16 to 8192
    /// Cascades, 1 to 4 (default 1: the single map of the first slice, unchanged). Several split the view range [near, distance] by the practical
    /// scheme (`split_lambda`: 0 uniform, 1 logarithmic) into slices that each get a fitted, texel-snapped map of `map_size` (tiles of one atlas
    /// texture, 2048x2048 each by default), so near shadows are sharp and the range reaches farther. Over the last `cascade_blend` fraction of a
    /// slice its shadow blends into the next one's.
    uint32_t cascades = 1;
    float split_lambda = 0.75f;            ///< 0 to 1
    float cascade_blend = 0.1f;            ///< 0 to 0.5
    float distance = 60.0f;                ///< meters of view depth that receive shadows; beyond it the sun is unoccluded
    float caster_margin = 50.0f;           ///< how far toward the sun the light's near plane is pulled, so off-screen casters still shadow
    float depth_bias_constant = -1024.0f;  ///< rasterizer depth bias of the shadow pass (reverse-Z: negative moves casters away from the sun)
    float depth_bias_slope = -1.5f;
    float normal_offset_texels = 1.5f;     ///< receivers move this many shadow texels along their geometric normal before the lookup
    uint32_t filter_radius = 1;            ///< percentage-closer filter radius in texels: (2r + 1)^2 taps; 0 = a single tap
};

/// Named shadow qualities: what a host exposes in a settings menu. The values are the table in docs/rendering/shadows.md; a host that wants other numbers fills
/// SunShadowSettings itself. `distance` and the bias terms are left at their defaults.
enum class ShadowQuality : uint8_t { Low, Medium, High, Ultra };
[[nodiscard]] SunShadowSettings shadow_quality_preset(ShadowQuality quality, float distance = 60.0f);

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
    /// A single-scattering atmosphere (atmosphere.hpp). Off by default (a flat `sky` clear color, exactly as before). On: a sky pass draws the sky behind everything
    /// from the sun direction of `lighting` (the same normalized vector the lighting uses), and the fog color (and with it the models' ambient sky color) becomes the
    /// horizon color of that sky, so distant things fade into the sky they are seen against. In an LDR scene the radiance goes through 1 - exp(-x); in HDR it is left
    /// to the tone map. Fixed at construction; the sun direction and the atmosphere do not change while the renderer lives (see docs/rendering/atmosphere.md).
    AtmosphereSettings atmosphere;
    /// Per-frame lighting (time of day). **Off by default**: `lighting` and the atmosphere's sun are fixed at construction, exactly as before. On: `SceneFrame::lighting`
    /// replaces the sun direction, sun color and ground ambient of that frame, and everything that depends on the sun follows it, from one validated value: the sky pass,
    /// the fog and clear color (the horizon of that sky when the atmosphere is on), the models' lighting and ambient sky color, and the shadow cascades' fit. A frame
    /// that supplies nothing uses `lighting`. Costs one small uniform buffer and bind group per frame in flight in the model renderer. Composes with shadows, IBL, skinning and HDR.
    /// With the sun at or below the horizon (`z <= 0`) no cascades are fitted that frame: nothing is shadowed by a sun that is not up. See docs/rendering/dynamic-lighting.md.
    bool dynamic_lighting = false;
    /// Image-based lighting for imported models with metallic-roughness shading (model_shading must be MetallicRoughness, else the model renderer throws
    /// std::invalid_argument when it is created). Null is off and changes nothing; see ModelRenderSettings::environment.
    std::shared_ptr<const EnvironmentLighting> environment;
    /// GPU skinning for imported models (`ModelRenderSettings::skinning`): skinned primitives, `ModelRenderer::create_skin_palette` and `ModelDraw::palette`.
    /// Off by default, and then nothing is allocated for it.
    bool skinning = false;
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
    /// One effective lighting per device frame: the first `render` of a frame fixes it, whether or not it draws models, and another `render` in the same device frame (a second
    /// view) must supply the same lighting (the same normalized sun, sun color and ground ambient) or it is refused with std::logic_error before anything is created, recorded
    /// or written. The next frame starts fresh.
    /// This frame's sun and ambient (needs `RendererSettings::dynamic_lighting`; not retained after render()). `nullptr`: the construction-time `lighting`. Validated,
    /// like the settings' lighting, before anything is recorded or written: `render` throws std::invalid_argument for a bad value and std::logic_error if the renderer
    /// was not built with dynamic lighting, and nothing has changed when it does.
    const SceneLighting* lighting = nullptr;
};

/// The lighting for a sun at `to_sun` (any non-zero direction; normalized), derived from the daytime look `day` and the atmosphere: the sun's color is `day.sun_color`
/// times the fraction of sunlight the air lets through (reddening toward the horizon, zero once the sun is below it), and the ground ambient fades from `day` at high
/// sun down to `night_ambient` (a fraction, default 0.08) as the sun sets, smoothly over sin(elevation) from -0.1 to 0.35. Pure and deterministic: what a host passes
/// as `SceneFrame::lighting` while it advances a clock, with `solar_position` supplying `to_sun`. Throws std::invalid_argument for a bad direction or fraction.
[[nodiscard]] SceneLighting time_of_day_lighting(const AtmosphereSettings& atmosphere, Float3 to_sun, const SceneLighting& day, float night_ambient = 0.08f);

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
    uint32_t model_skinned_draws = 0;      ///< draw calls of skinned primitives, shadow pass and scene pass together
    uint32_t model_skinned_skipped = 0;    ///< skinned primitives not drawn (skinning off, or the palette has too few joints)
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
    // The sky pass, created on the first frame that needs it (so a failure there is a failed frame, not a half-built renderer).
    rhi::ShaderHandle sky_vs_, sky_ps_;
    rhi::PipelineHandle sky_pipeline_;
    void ensure_sky();
    // Dynamic lighting: the one effective lighting of the current device frame (set by its first render; a later render of the same frame must supply the same lighting).
    struct LightingSnapshot {
        Float3 sun{};
        Float3 sun_color{};
        Float3 ambient_ground{};
    };
    uint64_t lighting_snapshot_frame_ = ~uint64_t{0};
    LightingSnapshot lighting_snapshot_;
    uint64_t lighting_published_frame_ = ~uint64_t{0};  ///< the device frame for which this renderer published the models' lighting
    [[nodiscard]] Camera sky_camera(const SceneFrame& frame) const;  ///< the camera the frame is drawn with (the explicit one, or the follow/fixed camera's)
    [[nodiscard]] Float3 atmosphere_fog(Float3 to_sun) const;  ///< the horizon color the fog fades to when the atmosphere is on, display-referred in an LDR scene
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
