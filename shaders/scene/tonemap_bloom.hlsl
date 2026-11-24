// The tone-map pass with bloom: tonemap.hlsl compiled with SAIGE_BLOOM = 1. The bloom texture (the chain's finest level, half size) is sampled with a linear
// sampler and added to the scene color, scaled by the intensity, before exposure and the operator: tone_map(hdr + intensity * bloom). Set 0 gains
// 2 bloom texture and 3 linear sampler; the push block is unchanged (its pad is the intensity).
//
// @shader   SceneToneMapBloom
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_BLOOM 1
#include "tonemap.hlsl"
