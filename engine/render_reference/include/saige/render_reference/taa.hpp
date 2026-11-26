// Temporal anti-aliasing, CPU reference.
//
// Per frame: the projection is shifted by a sub-pixel jitter from a Halton(2, 3) sequence, so successive frames sample different positions inside each pixel. The resolve reprojects the previous frame's output
// with the per-pixel motion vector, clips the history sample to the colour range of the current frame's 3x3 neighbourhood in YCoCg (variance clipping bounded by the neighbourhood's min and max), and blends
// it with the current sample. The clip is what removes ghosting: a history colour that the current neighbourhood cannot explain is pulled toward the colours it can. The guarantee is on the neighbourhood's min/max
// box (the variance box is always inside it): the clipped history and the current sample both lie in that box, so their blend does too. A reset (camera cut), a history sample from outside the frame, or a zero-variance neighbourhood that disagrees with the history
// all fall back to the current frame. Motion vectors are in pixels and point from the current position to where the surface was in the previous frame.
//
// This is the oracle the shader will be compared against; it is not an optimised implementation and says nothing about GPU cost.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace saige::render_reference {

/// The radical inverse of `index` (>= 1) in `base` (2 or more): the Halton sequence value in [0, 1).
[[nodiscard]] double halton(unsigned index, unsigned base);
/// The sub-pixel jitter of frame `frame` from a Halton(2, 3) sequence of `length` samples (1 to 1024), each component in [-0.5, 0.5).
[[nodiscard]] std::array<double, 2> jitter_offset(unsigned frame, unsigned length = 8);
/// The clip-space (NDC) translation that applies a pixel-space jitter to a projection of a `width` x `height` target: x is +2 jx / width, y is -2 jy / height (NDC y points up, pixel y down).
[[nodiscard]] std::array<double, 2> jitter_ndc(const std::array<double, 2>& jitter_pixels, unsigned width, unsigned height);
/// The texture LOD bias that keeps texture sharpness when rendering at `render_width` and presenting at `output_width`: log2(render / output) (negative when upscaling).
[[nodiscard]] double texture_lod_bias(unsigned render_width, unsigned output_width);

/// A linear-light RGB image, row-major, three floats per pixel.
struct Image {
    unsigned width = 0, height = 0;
    std::vector<float> rgb;
    Image() = default;
    Image(unsigned w, unsigned h);
    [[nodiscard]] std::array<float, 3> at(unsigned x, unsigned y) const;
    void set(unsigned x, unsigned y, const std::array<float, 3>& c);
};

/// Per-pixel motion in pixels, previous position minus current position, two floats per pixel.
struct MotionField {
    unsigned width = 0, height = 0;
    std::vector<float> mv;
    MotionField() = default;
    MotionField(unsigned w, unsigned h, float mx = 0.0f, float my = 0.0f);
    [[nodiscard]] std::array<float, 2> at(unsigned x, unsigned y) const;
    void set(unsigned x, unsigned y, float mx, float my);
};

struct TaaParams {
    float blend = 0.1f;          ///< weight of the current frame at rest, in (0, 1]
    float motion_blend = 0.05f;  ///< extra current-frame weight per pixel of motion (the history is less trustworthy when it moves far)
    float gamma = 1.25f;         ///< variance clip: the box is mean +- gamma * sigma, bounded by the neighbourhood min/max
    bool clip_history = true;    ///< off only for tests and ablation; without it the history ghosts
};

/// One resolve. `history_valid` false (a camera cut or the first frame) returns the current frame. The images and the motion field must have the same size (throws std::invalid_argument otherwise).
[[nodiscard]] Image taa_resolve(const Image& current, const Image& history, const MotionField& motion, const TaaParams& params, bool history_valid = true);

/// RGB <-> YCoCg (linear transforms; Y = (R + 2G + B) / 4, Co = (R - B) / 2, Cg = (-R + 2G - B) / 4).
[[nodiscard]] std::array<float, 3> to_ycocg(const std::array<float, 3>& rgb);
[[nodiscard]] std::array<float, 3> from_ycocg(const std::array<float, 3>& ycocg);

} // namespace saige::render_reference
