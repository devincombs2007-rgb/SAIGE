// The model shader with local lights and image-based lighting (bindings 6 to 8): model.hlsl compiled with SAIGE_LOCAL_LIGHTS = 1 and the same defines as the matching
// permutation without lights. Set 1 gains binding 9 (the frame's light records) and 10 (count and capacity) beside the other bindings. Only metallic-roughness
// shading reads the lights. docs/rendering/local-lights.md.
//
// @shader   SceneModelLitIbl
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_LOCAL_LIGHTS 1
#define SAIGE_IBL 1
#include "model.hlsl"
