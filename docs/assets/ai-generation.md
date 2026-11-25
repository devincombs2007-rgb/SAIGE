# AI-generated assets: what each service gives us

Prefer professional existing assets; generate only to fill a gap. Nothing in this file has been called: the hosts are outside this session's network allow-list and nothing may be spent without the owner. Facts below
come from each service's own documentation or help pages (sources in `integration-report.md`); where sources disagreed that is said. **A free website account is not a free API, and neither is a commercial licence.**

| Service | What it offers | API / agent access | Free? | Commercial use of output | Verdict |
| --- | --- | --- | --- | --- | --- |
| **Tripo AI** | text/image to textured 3D, rigging, retopology | REST OpenAPI platform (Bearer key); balance endpoint is free to read | web free plan with monthly credits; API credits are separate and the sources conflict (an announcement said 2,000 on first key, trackers say none) | free-plan output is CC BY 4.0, no commercial rights; paid plans include commercial use | usable for prototypes once the owner has looked at the API dashboard; no spend without approval |
| **Meshy AI** | text/image to 3D, texturing | REST API on **paid plans only** (Pro, about $20/month, plus prepaid credits) | the free plan is the web app only | per plan | **not usable without paying**; skipped |
| **Higgsfield AI** | concept art, character references, video | official **MCP server** (`https://mcp.higgsfield.ai/mcp`, OAuth, no key) and a separate REST API (key id + secret, own dollar balance) | everything through MCP deducts credits | its Terms of Use | useful for concept art and cinematic planning, not as an asset source; every call costs credits |
| **ElevenLabs** | sound effects, voices, dialogue, narration | REST API (`xi-api-key`) | the free plan includes monthly credits (reports of free-key `401 detected_unusual_activity` exist) | free: non-commercial and attribution required; paid plans include a commercial licence | prototypes and gap-filling; shipping voices needs the owner's decision on a paid plan |
| **Mixamo** (Adobe) | auto-rigging of humanoids and a large mocap animation library (FBX) | **no public API**; browser with an Adobe login only; scripted downloading is not sanctioned | free with an Adobe account | commercial use of the animated characters is allowed; **redistributing the raw files is not**; Adobe's terms bar using the content to train AI | manual download by the owner, kept in the private repository only |
| **TRELLIS** (Microsoft), **TripoSR**, **InstantMesh**, **Hunyuan3D-2** (Tencent) | open image/text to 3D | run locally | free | TRELLIS is MIT; TripoSR MIT; InstantMesh Apache-2.0; Hunyuan3D-2 is the Tencent Community Licence (territory and scale conditions: read it) | need a **GPU** (TRELLIS about 16-24 GB VRAM, Hunyuan3D-2 about 6 GB for shape and 16 GB with texture): **this environment has no GPU**, and `huggingface.co` is not allowed; not runnable here |

## Where AI generation fits the workflow

Step 9 of `architecture.md`: only after the library and the approved sources have nothing suitable. A generated model is a *candidate* like any other: record the service, the prompt, the date and the licence
terms of the plan used; run `saige-assets validate` (generated meshes are often dense, un-UV'd or non-manifold); retopologise and bake with Blender before it is called game-ready; keep CC-BY or non-commercial
output out of anything we plan to ship commercially until the owner has chosen a plan.

## Recommended use

1. **Characters:** MakeHuman community assets (CC0/CC-BY, about 550 CC0 clothes, 770 CC0 materials, poses, rigs) through Blender, then Mixamo animations downloaded once by the owner, before any generated character.
2. **Props and set dressing:** Poly Haven (hundreds of CC0 scanned models), Khronos samples, Sketchfab CC0/CC-BY with the key; Tripo only for one-off hero props the libraries lack.
3. **Concept references:** Higgsfield through its MCP server (credits permitting) to settle art direction before modelling.
4. **Audio gaps:** ElevenLabs sound effects as prototypes; replace with recordings.
