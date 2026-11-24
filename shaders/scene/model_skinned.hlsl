// The model vertex stage with linear blend skinning: model.hlsl compiled with SAIGE_SKINNING = 1, vertex stage only. The pixel stage is unchanged and is
// the one of whichever model permutation is in use (plain, shadowed, IBL, both): the vertex outputs are identical. Vertex input gains location 4
// (RGBA16_UINT joints) and 5 (RGBA32_FLOAT weights); set 2 binding 0 is the joint palette.
//
// @shader   SceneModelSkinned
// @entry    VSMain vs

#define SAIGE_SKINNING 1
#include "model.hlsl"
