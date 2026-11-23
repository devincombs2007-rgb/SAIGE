// The model shader with sun shadows: model.hlsl compiled with SAIGE_SUN_SHADOWS = 1 (see its header). Same entry points, push constants and
// set 0; set 1 gains the sun shadow map, its comparison sampler and the per-frame shadow block.
//
// @shader   SceneModelShadowed
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_SUN_SHADOWS 1
#include "model.hlsl"
