# Gemini engineering transfer: immutable upstream copies (received 2026-11-27)

**Do not edit anything under `upstream/gemini/`.** These are the files exactly as delivered. Engine code that derives from them lives in the normal source tree and records what it changed and why.

## The archive
- `SAIGE_Claude_Engineering_Transfer_Package.zip`, 3,954,935 bytes, SHA-256 `9910fa465dfcad1ab1f9c571311bb9ca3b6e4219c34cd2e558d8cbffaf56068d`, relayed by the owner to session `session_01Ss7dAaepZA6pgXb3dyjoBW`; inventory recorded on #43 in comment 7209863396; ownership transferred to Claude by owner directives 7209812710 and 7209863396.
- Verified on receipt: all 90 entries of `TRANSFER_MANIFEST.json` match (size and SHA-256); nested manifests match (SSR 24/24, World Editor 27/27, World Simulation 36/36). No executables, no symlinks.

## What is here and where the rest is
| Division | Build | Location | Status on receipt |
| --- | --- | --- | --- |
| World Editor | 012 | `upstream/gemini/world_editor_build012/` (all 27 files) | New: no editor code existed anywhere in the repository (checked every live branch). |
| World Simulation | 010 | `upstream/gemini/world_simulation_build010/` (all 36 files) | Five modules (`crowd_avoidance`, `multi_floor_nav`, `npc_simulation`, `traffic_simulation`, `saige_world_adapter`) are **byte-identical** to the copies already in `include/saige/` and `src/` on `claude/fidelity-dev`, `claude/s04-quality`, `claude/ssr-validation` and `claude/showcase-04`. Four are new: `local_economy`, `npc_psychology`, `persistent_world`, `social_simulation` (plus tests and a demo). Targeted base: `claude/production-infrastructure` `d15bfd3` (manifest). |
| SSR | 002.1 | not duplicated here: the 22 text files are **byte-identical** to `validation/ssr/upstream/` on `claude/ssr-validation` | The corrected validation harness there (`61c37b42c4cac7b12b7a1e07588c0847d2784c54`) stays authoritative; the delivered harness must not replace it. |

The SSR media are kept out of Git history (size); their hashes, from the verified manifest:
- `ssr/visuals/ssr_1080p_day_comparison.png`: 409752 bytes, SHA-256 `fb2c120ef1519e5a55b8ee4833a6a592bceefde76875d7ad13c94f7d7b1e13c3`
- `ssr/visuals/ssr_1080p_night_hdr_comparison.png`: 49053 bytes, SHA-256 `f1a369a39b5edec366bf38463e46b25a15093415e7eb36d936f069366e8337d1`
- `ssr/visuals/ssr_720p30_diagnostic.mp4`: 3270972 bytes, SHA-256 `42d0560d8a6b4529494f332e35c28875ad0296e95f7be2850c3a27b779b36093`

## Known findings that apply to this code (carried over, not re-derived here)
- SSR: Gemini's algorithm findings A1–A4 and the failing rapid pan-and-cut gate (34.74 dB against 35) recorded on `claude/ssr-validation`; production SSR blocked.
- World Editor: director review 7198676267 (material finite/range validation, world transforms and the +X forward axis, cross-chunk clearance, dependencies and licensing, vehicle attachments, versioned persistence, a validator bridge). Build 012 predates it; a Build 013 was never delivered.
- World Simulation: standalone and experimental; must be reconciled with the current navigation, traffic and persistence before integration (exact ledger, event priority, crowd/navigation behaviour, boundary conditions).
- The packages' `HANDOFF.md` files describe the old ownership split; owner directive 7209812710 supersedes them.
