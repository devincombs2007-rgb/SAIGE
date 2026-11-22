// The embedded environment BRDF table. The data is generated: see env_brdf.hpp and tools/envbrdf_gen.cpp.
#include "saige/render/env_brdf.hpp"

#include <cstdint>
#include <iterator>

namespace saige::render {

namespace {
#include "generated/env_brdf_table.inc"
} // namespace

const EnvBrdfAsset& precomputed_env_brdf()
{
    static const EnvBrdfAsset asset{kEnvBrdfTexelsVersion, kEnvBrdfTexelsSize, kEnvBrdfTexelsSteps, kEnvBrdfTexelsChecksum,
                                    kEnvBrdfTexels, std::size(kEnvBrdfTexels)};
    return asset;
}

} // namespace saige::render
