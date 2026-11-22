// Structural guard for the model pixel shader's use of derivatives. Implicit-LOD sampling takes its derivatives from the other
// invocations of the 2x2 quad; Vulkan leaves them undefined in non-uniform control flow and for neighbours that were discarded,
// and a software rasterizer does not expose that (llvmpipe computes the quad gradients regardless). So the property is checked on
// the generated SPIR-V itself: the base color sample (taken in uniform flow, before the alpha-mask discard) is the only
// implicit-LOD sample, and every slot read after it uses explicit gradients or an explicit level.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/scene_model.spv.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>

using namespace saige;

namespace {

constexpr uint32_t kOpImageSampleImplicitLod = 87;
constexpr uint32_t kOpImageSampleExplicitLod = 88;
constexpr uint32_t kOpImageSampleDrefImplicitLod = 89;
constexpr uint32_t kOpImageSampleProjImplicitLod = 91;
constexpr uint32_t kOpImageSampleProjDrefImplicitLod = 93;
constexpr uint32_t kOpKill = 252;
constexpr uint32_t kOpTerminateInvocation = 4416;
constexpr uint32_t kOpDemoteToHelperInvocation = 5380;
constexpr uint32_t kOpDPdx = 207;
constexpr uint32_t kOpDPdy = 208;

struct Scan {
    int implicit_samples = 0;
    int explicit_samples = 0;
    int derivative_ops = 0;
    long first_implicit = -1;
    long last_implicit = -1;
    long first_kill = -1;
    long first_derivative = -1;
};

template <std::size_t N>
Scan scan(const uint32_t (&words)[N])
{
    Scan s;
    REQUIRE(N > 5);
    REQUIRE(words[0] == 0x07230203u);
    std::size_t i = 5;  // after the header
    long index = 0;
    while (i < N) {
        const uint32_t opcode = words[i] & 0xFFFFu;
        const uint32_t count = words[i] >> 16;
        REQUIRE(count != 0);
        switch (opcode) {
            case kOpImageSampleImplicitLod:
            case kOpImageSampleDrefImplicitLod:
            case kOpImageSampleProjImplicitLod:
            case kOpImageSampleProjDrefImplicitLod:
                ++s.implicit_samples;
                if (s.first_implicit < 0) s.first_implicit = index;
                s.last_implicit = index;
                break;
            case kOpImageSampleExplicitLod:
                ++s.explicit_samples;
                break;
            case kOpKill:
            case kOpTerminateInvocation:
            case kOpDemoteToHelperInvocation:
                if (s.first_kill < 0) s.first_kill = index;
                break;
            case kOpDPdx:
            case kOpDPdy:
                ++s.derivative_ops;
                if (s.first_derivative < 0) s.first_derivative = index;
                break;
            default: break;
        }
        i += count;
        ++index;
    }
    return s;
}

} // namespace

TEST_CASE("Model pixel shader: only the base color sample is implicit-LOD, and it precedes the alpha-mask discard")
{
    const Scan s = scan(shaders::scene_model_ps_main);
    // Base color: the one implicit sample, in uniform control flow ahead of any discard.
    CHECK(s.implicit_samples == 1);
    if (s.first_kill >= 0 && s.last_implicit >= 0) {
        CHECK(s.last_implicit < s.first_kill);
    }
    // Metallic-roughness, normal, occlusion and emissive use SampleGrad; the environment BRDF table uses SampleLevel.
    CHECK(s.explicit_samples >= 5);
    // The gradients are computed once from the interpolated UV, ahead of the discard and the tangent-validity branch.
    CHECK(s.derivative_ops >= 2);
    if (s.first_kill >= 0 && s.first_derivative >= 0) {
        CHECK(s.first_derivative < s.first_kill);
    }
}
