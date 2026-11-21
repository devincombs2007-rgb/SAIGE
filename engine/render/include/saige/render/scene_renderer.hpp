// SAIGE Render — the 0.1 scene renderer: a procedural checkerboard ground plus one lit cube per instance.
//
// Backend-agnostic: records into any rhi::CommandList and derives barriers with the RenderGraph. The frame
// sink adapter (engine/render_sink) feeds it from Core's FrameView.
#pragma once

#include "saige/render/camera.hpp"
#include "saige/render/math.hpp"
#include "saige/render/render_graph.hpp"
#include "saige/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
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

struct RendererSettings {
    CameraMode camera_mode = CameraMode::Follow;
    Float3 follow_offset{-6.0f, -4.5f, 3.5f};  ///< behind (-X), to the right (-Y) and above the target
    Float3 fixed_eye{-6.0f, -4.5f, 4.0f};
    Float3 fixed_target{0.0f, 0.0f, 0.5f};
    float fov_y_radians = 60.0f * pi / 180.0f;
    float near_plane = 0.1f;
    rhi::ClearColor sky{0.30f, 0.50f, 0.82f, 1.0f};  ///< linear; also the fog color
    float fog_density = 0.012f;                      ///< per meter
    float ground_half_size = 150.0f;                 ///< meters, centered under the camera target
    float cube_edge = 1.0f;                          ///< meters
    float spin_radians_per_second = 0.8f;
    double seconds_per_tick = 1.0 / 60.0;           ///< matches Core's default fixed step
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

struct FrameStats {
    uint32_t draw_calls = 0;
    uint32_t barriers = 0;
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
    [[nodiscard]] rhi::Format color_format() const noexcept { return color_format_; }

private:
    void ensure_depth(uint32_t width, uint32_t height);
    [[nodiscard]] Float3 camera_target(const SceneFrame& frame) const;

    rhi::Device& device_;
    rhi::Format color_format_;
    RendererSettings settings_;
    rhi::ShaderHandle cube_vs_, cube_ps_, ground_vs_, ground_ps_;
    rhi::PipelineHandle cube_pipeline_, ground_pipeline_;
    rhi::BufferHandle cube_vertices_, cube_indices_;
    rhi::TextureHandle depth_;
    uint32_t depth_width_ = 0;
    uint32_t depth_height_ = 0;
    rhi::ResourceState depth_state_;
    RenderGraph graph_;
};

} // namespace saige::render
