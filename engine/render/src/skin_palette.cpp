#include "saige/render/skin_palette.hpp"

#include "saige/render/model.hpp"

#include <cmath>
#include <stdexcept>

namespace saige::render {

void SkinPalette::update(std::span<const Mat4> matrices)
{
    if (!valid()) {
        throw std::logic_error("SkinPalette::update: the palette is empty (create it with ModelRenderer::create_skin_palette)");
    }
    if (matrices.size() != joint_count_) {
        throw std::invalid_argument("SkinPalette::update: one matrix per joint is required");
    }
    // Check everything first so a bad matrix leaves the previous pose untouched.
    for (const Mat4& m : matrices) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(m.at(r, c))) {
                    throw std::invalid_argument("SkinPalette::update: a skin matrix is not finite");
                }
            }
        }
        const float expected[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(m.at(3, c) - expected[c]) > 1e-3f) {
                throw std::invalid_argument("SkinPalette::update: a skin matrix is not affine (the bottom row must be 0 0 0 1)");
            }
        }
    }
    for (std::size_t j = 0; j < matrices.size(); ++j) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                rows_[j * 12 + static_cast<std::size_t>(r) * 4 + static_cast<std::size_t>(c)] = matrices[j].at(r, c);
            }
        }
    }
    ++generation_;
}

} // namespace saige::render
