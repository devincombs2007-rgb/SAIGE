// The first bloom downsample: bloom_down.hlsl with the soft-knee threshold applied to every tap (SAIGE_BLOOM_PREFILTER = 1).
//
// @shader   SceneBloomPrefilter
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_BLOOM_PREFILTER 1
#include "bloom_down.hlsl"
