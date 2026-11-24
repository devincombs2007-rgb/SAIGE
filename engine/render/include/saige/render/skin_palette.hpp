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

    /// The GPU pose is **frozen for a frame on its first use**: once a draw (colour, sorted or shadow pass) of this renderer frame has bound the palette, the frame's draws
    /// reference that copy of the pose, so `update` between draws of the same frame would change what the recorded ones show. `ModelRenderer::draw` and `draw_shadow` therefore
    /// refuse, with std::logic_error and before recording or writing anything, a palette that was updated after its first use in the current device frame. Update every
    /// palette before the frame's first draw (any number of times; the last pose counts), and give each view or character its own palette if poses differ.
    /// The uniform buffer of frame slot `slot` (for tests and diagnostics); invalid when out of range. Read it only when the device has finished the slot's frames.
    [[nodiscard]] rhi::BufferHandle slot_buffer(uint32_t slot) const noexcept { return slot < buffers_.size() ? buffers_[slot].handle() : rhi::BufferHandle{}; }

private:
    friend class ModelRenderer;

    rhi::Device* device_ = nullptr;
    uint32_t joint_count_ = 0;
    std::vector<float> rows_;          ///< kMaxSkinJoints * 12 floats: the three rows of each joint's matrix; joints beyond joint_count are zero
    uint64_t generation_ = 0;          ///< bumped by every update; a frame slot is current when its generation matches
    std::vector<BufferRef> buffers_;   ///< one uniform buffer per frame in flight
    std::vector<BindGroupRef> groups_; ///< set 2 of the skinned pipelines, one per slot, binding the slot's buffer
    mutable std::vector<uint64_t> slot_generation_;
    mutable std::vector<uint64_t> slot_frame_;  ///< the device frame that last bound the slot (the pose it holds is frozen for that frame)
};

} // namespace saige::render
