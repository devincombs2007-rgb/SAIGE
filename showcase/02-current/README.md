# Showcase 02 · Courtyard

This capture branch reuses Showcase 01's capsule course, glTF scene, offscreen Vulkan readback and deterministic camera path.
The footage uses the #91 dynamic-lighting development branch and #89's corrected cascade-atlas sampling. The character is
the rigid course avatar driven by SAIGE's capsule controller. Three cascades, PBR, HDR output and atmospheric scattering
are enabled. The video edit adds titles and an original ambient score.

The courtyard, character and collision are from Showcase 01. The selected Poly Haven CC0 crates, barrels, plants and
terrain color texture are credited in [assets.json](assets.json). The distant landscape mesh is generated locally.
The video uses one resident courtyard, without streaming, vehicle, GPU skinning or HDR image-based reflections.

## Capture on Linux

Install a Vulkan 1.3 driver with validation layers, Python with NumPy, FFmpeg with libx264 and DejaVu Sans.
Use the pinned asset files and renderer validation for the full cut.

```sh
python3 showcase/02-current/fetch_assets.py /tmp/saige-environment
python3 showcase/02-current/landscape.py /tmp/saige-environment/landscape.gltf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON -DSAIGE_BUILD_PROJECTS=OFF
cmake --build build --target saige_showcase
SAIGE_REQUIRE_VALIDATION=1 python3 showcase/02-current/export.py --exe build/showcase/saige_showcase --environment /tmp/saige-environment --out capture --width 1152 --height 648
```

The renderer produces 1,800 distinct capture frames at 1152×648 and 30 frames per second of replay time.
The edit scales to 1920×1080 and exports at 60 FPS with repeated frames. The number describes video playback.
Software Vulkan and offline rendering provide no evidence of physical GPU performance.
