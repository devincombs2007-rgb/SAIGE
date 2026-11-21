# SAIGE 0.2 roadmap

The milestone is a native Windows x64 sample that opens a versioned project containing an original textured glTF 2.0 scene. It displays multiple objects at their authored transforms with correct depth and lets the user fly a perspective camera. Linux and Windows regression lanes verify software Vulkan. Physical Windows GPU behavior has separate acceptance evidence.

| Area | Pass condition | Owner |
| --- | --- | --- |
| Hierarchy | Parent chains resolve independent of document order. Shared mesh instances retain different local transforms. Test parent edits, deep chains, invalid parents, cycles and shear from nonuniform scale. | Core |
| Camera | WASD translates relative to the view and mouse drag rotates. Test focus loss, resize, and a scripted input trace. Keep a positive near plane and verify occlusion after projection or aspect changes. | Core input and camera pose, Claude projection |
| Assets | Load an original glTF fixture with indexed triangles, multiple mesh instances and a texture. Test shared references, path containment and actionable failures for malformed files or unsupported required extensions. | Claude import and GPU upload, Core manifest |
| Persistence | Store stable scene and asset IDs, hierarchy, local poses and camera parameters with a schema version. Round-trip all supported fields. A failed load leaves the live project intact. | Core |
| Rendering | Display the imported scene on Windows. Pixel tests detect texture variation, correct occlusion independent of draw order, and a changed view after camera movement. Validation reports zero errors during render, resize and shutdown. | Claude rendering, Core integration |
| Lifetimes | Reuse immutable GPU assets across instances. Reload a scene with frames in flight and confirm resources stay valid until their last submission finishes. Report live resources at teardown. | Claude |
| Diagnostics | Emit separate versioned JSON with CPU frame and extraction percentiles, counts, tracked resource bytes and GPU timestamp durations when supported. Report unavailable GPU timestamps explicitly. Record device, scene and build type. | Shared |

Start with the hierarchy resolver and [snapshot reuse](https://github.com/devincombs2007-rgb/SAIGE/pull/16). Next add the project manifest, persistence and input alongside Claude's importer and projection. Integrate the full world transform and camera at the frame boundary in [RFC #17](https://github.com/devincombs2007-rgb/SAIGE/issues/17). Preserve the 0.1 procedural demo and v1 CLI while adding the scene sample.

Local poses use normalized XYZW quaternions, positive scale, meters and right-handed Z up. The world affine matrix is authoritative because composition can introduce shear. Persistent IDs are separate from dense indices and generation-checked entity handles. Importers convert coordinates once. The project declares 128-bit asset IDs and project-relative resource paths. Rendering owns device resources, resource retirement and GPU encoding.

Use synchronous load and one simulation thread until measurements justify changes. Stable IDs support eventual streaming and world editing. The extracted frame boundary permits later parallel work. Keep a separate path to camera-relative coordinates for larger worlds. Define which system writes a transform before integrating dynamic physics bodies. Measure extraction, upload and device costs on small and larger fixtures before adding scheduling or allocator layers.
