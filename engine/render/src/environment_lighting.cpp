#include "saige/render/environment_lighting.hpp"

#include "saige/rhi/resource_desc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::render {

void validate_environment_lighting(const EnvironmentLighting& e)
{
    const auto bad = [](const char* what) { throw std::invalid_argument(std::string("EnvironmentLighting: ") + what); };
    if (e.width < 2 || e.height < 1 || e.width > 16384 || e.height > 16384) bad("the texture extent must be at least 2x1 and at most 16384 per side");
    if (e.levels < 1 || e.levels > rhi::max_mip_levels(e.width, e.height)) bad("the level count must be between 1 and the length of the full mip chain");
    std::size_t texels = 0;
    for (uint32_t i = 0; i < e.levels; ++i) texels += static_cast<std::size_t>(std::max(1u, e.width >> i)) * std::max(1u, e.height >> i);
    if (e.rgba16f.size() != texels * 4) bad("the texel count does not match the levels (four half floats per texel, every level)");
    for (const Float3& c : e.sh) {
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) bad("a spherical-harmonics coefficient is not finite");
    }
    if (!std::isfinite(e.intensity) || e.intensity < 0.0f) bad("the intensity must be finite and not negative");
}

} // namespace saige::render
