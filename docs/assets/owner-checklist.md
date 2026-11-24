# Owner checklist

**Default: do nothing.** The pipeline already works without any account: Poly Haven, ambientCG, Khronos samples, MakeHuman community assets and OpenGameArt (including real recorded CC0 sound effects) are
fetched by script, validated against the engine's importer and catalogued with no action from you. There is no connector or login route that lets an agent use your Freesound, ElevenLabs, Tripo, Meshy,
Higgsfield, Sketchfab or Mixamo accounts by itself (the only connector found for any of them is an ElevenLabs *voice-agent manager*, which does not generate sound effects), so each of those needs either
an API key you create once or a manual download. They are all optional; here they are ranked by how much they would improve quality for how little effort.

| Rank | What | Effort | Quality gain | Needed? |
| --- | --- | --- | --- | --- |
| 1 | Freesound API key (A2 + A6 below) | about 5 minutes once | a much larger pool of recorded vehicle, impact, foley and ambience sounds than the OpenGameArt packs | only if the current recorded sounds prove too thin |
| 2 | Mixamo animations (B1) | 15-20 minutes on a computer | real motion-captured human animation (the showcase character only has a walk cycle) | only when character animation becomes the priority |
| 3 | Sketchfab token (A5) | about 3 minutes once | CC0/CC-BY vehicles, which Poly Haven barely has | only when we need more cars |
| 4 | everything else (ElevenLabs, Tripo, Meshy, Higgsfield, GPU, Sonniss) | | small or paid | **skip** |

If you do nothing, I carry on with the keyless sources. Do these only if you want the quality jump. Never paste a key into a chat or a GitHub file: keys go only into the secure card or the environment
settings described below. Do not buy anything.

## A. Optional one-time setup (each item stands alone)

- [ ] **A1. Private assets repository** (for Mixamo files and anything that must not be public).
  GitHub app → your profile → Repositories → **New** → name `saige-assets-private` → **Private** → Create.
  Then tell me: **"private repo created"**. I attach it to the session and put a README and the catalog layout in it.

- [ ] **A2. Freesound key** (unlocks recorded sound effects: engines, tyres, impacts, foley, ambience). Free.
  Safari → `freesound.org` → log in → open `freesound.org/apiv2/apply` → **Create new API credentials** → name `saige`, description "game engine asset search", non-commercial/for development → submit.
  Keep the page open: you will copy the **Client secret / API key** into a secure card in step A6.

- [ ] **A3. ElevenLabs key** (voices and gap-filling sound effects; the free plan is non-commercial). Free.
  Safari → `elevenlabs.io` → profile icon → **API Keys** → **Create API Key** → name `saige`; if it offers restrictions, allow only *Sound Generation* and *User (read)*, and set a monthly credit limit.

- [ ] **A4. Tripo key** (custom 3D props only when nothing existing fits). Free credits only.
  Safari → `platform.tripo3d.ai` → **API Keys** → generate. Do not buy credits.

- [ ] **A5. Sketchfab token** (CC0/CC-BY vehicle and prop models).
  Safari → `sketchfab.com` → log in → **Settings** → **Password & API** → copy the API token.

- [ ] **A6. Save the keys securely** (one tap each). After A2-A5 say **"keys ready"**: I open one secure *Add credential* card per service in the Claude app; you paste the key into the card. Saving also allows that service's website in the session's network policy.
  Manual alternative: Claude app → this session's title bar → cloud environment (`SAIGE assets`) → **Edit** → **Network secrets** → add one per host (`freesound.org`, `api.elevenlabs.io`, `api.tripo3d.ai`, `api.sketchfab.com`). Variable names the tools read: `FREESOUND_API_KEY`, `ELEVENLABS_API_KEY`, `TRIPO_API_KEY`, `SKETCHFAB_API_TOKEN`.

## B. Downloads only you can do (best on a computer; the iPhone works with "Request Desktop Website")

- [ ] **B1. Mixamo animations** (character animation library). Adobe login.
  `mixamo.com` → sign in → **Characters**: pick **Y Bot** (a neutral humanoid) → **Animations**: search and add these one by one: *Idle*, *Walking*, *Running*, *Sprint*, *Jump*, *Falling Idle*, *Landing*, *Turn Left*, *Turn Right*,
  *Crouch Idle*, *Crouch Walk*, *Standing To Sit*, *Sitting Idle*, *Sit To Stand*, *Driving* (or *Sitting Idle*), *Opening Door*, *Pushing*, *Picking Up*, *Looking Around*, *Hit Reaction*, *Death*.
  **Download** each as: Format **FBX Binary (.fbx)**, Skin **With Skin** for the first file and **Without Skin** for the rest, 30 FPS, **Keyframe Reduction: none**.
  Upload the files to `saige-assets-private` → `mixamo/` (GitHub web: *Add file → Upload files*; each file is under 25 MB). Say **"mixamo uploaded"**.
  Mixamo files stay in the private repository only (Adobe forbids redistributing them).

- [ ] **B2. Sonniss GameAudioGDC (optional, later).** The bundles are tens of GB and the site blocks scripts; skip until Freesound proves insufficient. If wanted: pick individual packs in a desktop browser at `sonniss.com/gameaudiogdc`, and upload only the chosen folders to the private repository.

## C. Decisions (reply with the number and yes/no; defaults in bold)

1. Test an **older MPFB (MakeHuman for Blender) release** that works with the installed Blender 4.0.2, or allow `download.blender.org` for Blender 4.2+? MPFB is third-party GPL add-on code, so I did not run it without your say-so. **Default: no, wait.**
2. Pay for **ElevenLabs** (commercial licence, about $5-6/month) so generated voices can ship? **No.**
3. Buy **Tripo or Meshy** credits or plans? **No.**
4. Provide or pay for a **GPU machine** for local 3D generation (TRELLIS/Hunyuan3D)? **No.**
5. Connect the **Higgsfield MCP connector** for concept art (Claude app → Settings → Connectors → add custom → `https://mcp.higgsfield.ai/mcp` → sign in; it deducts Higgsfield credits)? **No.**

## D. What I do after each step

| You do | I do |
| --- | --- |
| A1 | attach the private repo, define its catalog, and wire `SAIGE_PRIVATE_ASSETS_DIR` |
| A6 (Freesound) | search CC0 recordings for every class in `audio.md`, catalog and prepare them, rebuild Showcase 03's soundtrack from real recordings |
| A6 (Sketchfab) | find CC0/CC-BY vehicles and props, validate them against the engine importer |
| B1 | convert FBX to glTF, validate skeletons, retarget to the showcase character, hand clips to Codex's animation work |
| A6 (ElevenLabs, Tripo) | only check balances (no spending) until you approve a use |

Questions or a step that does not work on the phone: say which one and I will find another route.
