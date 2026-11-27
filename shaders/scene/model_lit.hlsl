// The model shader with local lights: model.hlsl compiled with SAIGE_LOCAL_LIGHTS = 1. Same entry points, push constants and set 0; set 1 gains
// binding 9 (the frame's light records, a storage buffer) and 10 (the light count and the buffer's capacity). Only metallic-roughness shading reads the lights; skinning
// and instancing are not combined with them (the renderer refuses those); model_lit_shadowed, model_lit_ibl and model_lit_ibl_shadowed add sun shadows and IBL. docs/rendering/local-lights.md.
//
// @shader   SceneModelLit
// @entry    VSMain vs
// @entry    PSMain ps

#define SAIGE_LOCAL_LIGHTS 1
#include "model.hlsl"
