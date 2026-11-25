"""Acquisition sources. Each module exposes `search(...)` (remote index, no download) and `fetch(asset_id, ...)` (download into the cache, return a record dict).

Reachable and scriptable without any account: polyhaven, ambientcg, khronos, opengameart (content pages). Needing a key: freesound, elevenlabs, tripo, meshy, sketchfab, higgsfield
(see keyed.py). Manual only (no script is possible or permitted): Mixamo, Sonniss, Pixabay audio, CGTrader, Kenney downloads (a browser session is needed).
"""
