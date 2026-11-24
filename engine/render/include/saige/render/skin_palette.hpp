// SAIGE Render — the pose of one animated instance on the GPU: the skin matrices of its joints, ready for the skinned model pipelines.
//
// Ownership (agreed with Core on #43): evaluating an animation into a pose and a pose into skin matrices is engine-level stateless sampling
// (gfx_assets/animation.hpp); playback, blending and state transitions belong to gameplay. Gameplay hands the renderer the final skin matrices of
// the frame through SkinPalette::update; the renderer owns the GPU copies.
//
// A palette is created by ModelRenderer::create_skin_palette. It keeps the latest matrices on the CPU and a GPU copy per frame in flight; a draw
// writes the current frame's copy if it is stale, so `update` may be called any number of times between frames (the last call wins) and the GPU's
// reads of an earlier frame are never raced. One palette serves every skinned primitive of the instance, in the color and shadow passes.
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace saige::rhi {
class Device;
}

namespace saige::render {

class ModelRenderer;

class SkinPalette {
public:
    SkinPalette() = default;
    SkinPalette(SkinPalette&&) noexcept = default;
    SkinPalette& operator=(SkinPalette&&) noexcept = default;
    SkinPalette(const SkinPalette&) = delete;
    SkinPalette& operator=(const SkinPalette&) = delete;

    [[nodiscard]] bool valid() const noexcept { return device_ != nullptr; }
    [[nodiscard]] uint32_t joint_count() const noexcept { return joint_count_; }

    /// Replaces the pose. `matrices` holds one skin matrix per joint (the joint's world transform times its inverse bind matrix, in the skinned mesh's
    /// space, as gfx::skin_matrices produces). They must be finite affine transforms (bottom row 0 0 0 1 within 1e-3): the shader stores three
    /// rows. Throws std::invalid_argument, changing nothing, if the count differs from joint_count() or a matrix is not finite and affine.
    void update(std::span<const Mat4> matrices);

private:
    friend class ModelRenderer;

    rhi::Device* device_ = nullptr;
    uint32_t joint_count_ = 0;
    std::vector<float> rows_;          ///< kMaxSkinJoints * 12 floats: the three rows of each joint's matrix; joints beyond joint_count are zero
    uint64_t generation_ = 0;          ///< bumped by every update; a frame slot is current when its generation matches
    std::vector<BufferRef> buffers_;   ///< one uniform buffer per frame in flight
    std::vector<BindGroupRef> groups_; ///< set 2 of the skinned pipelines, one per slot, binding the slot's buffer
    mutable std::vector<uint64_t> slot_generation_;
};

} // namespace saige::render
