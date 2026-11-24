// The model shader with image-based lighting and sun shadows: model.hlsl compiled with SAIGE_SUN_SHADOWS = 1 and SAIGE_IBL = 1. Same entry points, push constants and set 0;
// set 1 gains bindings 6 (the prefiltered environment), 7 (its sampler) and 8 (the SH coefficients and parameters), and 3 to 5 for the sun shadows.
// Only metallic-roughness shading reads the environment; the renderer refuses the permutation for the other modes.
//
// @shader   SceneModelIblShadowed
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_SUN_SHADOWS 1
#define SAIGE_IBL 1
#include "model.hlsl"
