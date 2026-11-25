# Licence matrix and redistribution rules

The engine repository is **public**. "Redistribution" below means committing the file there. The catalog records the facts per asset (`license.spdx`, `license.redistribution`, `license.attribution`); this page is the policy.

| Source | Licence of the assets | Public repo may hold the files? | Credit required in shipped product | Notes |
| --- | --- | --- | --- | --- |
| Poly Haven | CC0-1.0 | yes (but we re-fetch) | no (appreciated) | models, PBR textures, HDRIs; documented API |
| ambientCG | CC0-1.0 | yes (re-fetch) | no | PBR materials, HDRIs, some models |
| Khronos glTF Sample Assets | **per model** (CC0, CC BY 4.0, others) | CC0 and CC BY yes; anything else no | CC BY: yes | e.g. Car Concept and Cesium Man are CC BY 4.0: credit on the end card/credits |
| MakeHuman community assets | **per asset**: CC0, CC-BY or AGPL | CC0 and CC-BY yes; **AGPL: decide first** | CC-BY: yes | the MakeHuman application and MPFB add-on are GPL/AGPL software; characters exported from them are CC0 per the MakeHuman project's licence statement for output (verify when first used) |
| OpenGameArt | **per item** (CC0, CC-BY 3/4, CC-BY-SA, GPL, OGA-BY) | CC0 and CC-BY yes; SA/GPL no | CC-BY: yes | the tool reads each page's licence; unreadable means `unknown` |
| Kenney | CC0 | yes | no | browser download (the site's signed link needs a session); also mirrored on OpenGameArt |
| Quaternius | CC0 (per their site) | yes | no | download links are on pages not served as plain HTML here: browser download |
| Sketchfab | **per model** (CC0, CC-BY, CC-BY-SA, CC-BY-NC, Sketchfab standard ...) | CC0 and CC-BY only | CC-BY: yes | API download needs the owner's account token; the API terms require authenticated use |
| CGTrader (free section) | per model; royalty-free licence is **not** redistributable on its own | no | per model | login and a browser; no scripted download |
| Freesound | **per sound**: CC0, CC-BY, CC-BY-NC, sampling+ ... | CC0 and CC-BY only; **never NC** | CC-BY: yes | the API filters by licence; use `license:"Creative Commons 0"` by default |
| Sonniss GameAudioGDC | royalty-free, commercial use, **no standalone redistribution**; recent bundles forbid AI training use | no (embedded in a product only) | no | private repository only; read the licence shipped with each bundle |
| Pixabay audio | Pixabay Content License: free for commercial use, **no standalone redistribution**, some samples carry third-party rights | no | no | manual download; the API does not serve audio |
| Mixamo | Adobe terms: use in your own projects including commercial; **no redistribution of the files**; no AI training | **no** | no | private repository only; Adobe login in a browser |
| AI services | per plan (see `ai-generation.md`) | only if the plan grants commercial rights and the file is ours to publish | per plan | record the plan and date in the asset's `license.notes` |
| Showcase originals | ours | yes | no | procedural/engine fixtures |

Decision rule when a licence is not in the table: `unknown` → private → a person reads it → update the record. SA/GPL/AGPL content is kept out of the engine repository because it would impose its
licence on what it ships with; if a pack is wanted anyway, the owner decides.
