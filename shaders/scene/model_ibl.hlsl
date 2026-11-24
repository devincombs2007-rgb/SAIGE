// The model shader with image-based lighting: model.hlsl compiled with SAIGE_IBL = 1. Same entry points, push constants and set 0;
// set 1 gains bindings 6 (the prefiltered environment), 7 (its sampler) and 8 (the SH coefficients and parameters).
// Only metallic-roughness shading reads the environment; the renderer refuses the permutation for the other modes.
//
// @shader   SceneModelIbl
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_IBL 1
#include "model.hlsl"
