"""Services that need a key. Nothing here spends credits: `whoami()` calls are read-only account/quota endpoints, `search` calls are free, and every generating call (`generate_*`)
refuses to run unless the caller passes `confirm_spend=True`, which only a human-approved task may do. Keys come from environment secrets (never from files in git).

UNVERIFIED AGAINST THE LIVE SERVICES: the hosts are not in this session's network allow-list (see docs/assets/integration-report.md), so these clients follow each service's published
API documentation but have not been run. They are small on purpose; the first live run should be `saige-assets doctor --services`.
"""
import os

from ..common import http_get, UA


def _key(name):
    v = os.environ.get(name)
    if not v:
        raise RuntimeError(f"{name} is not set (save it as an environment secret; see docs/assets/owner-checklist.md)")
    return v


# ---- Freesound: https://freesound.org/docs/api/ (token auth for search/previews; OAuth2 for original-quality download)
def freesound_search(query, filter_license=("Creative Commons 0",), page_size=15, fields="id,name,username,license,duration,previews,tags,avg_rating,num_downloads,filesize,samplerate,channels"):
    lic = " OR ".join(f'license:"{x}"' for x in filter_license)
    url = f"https://freesound.org/apiv2/search/text/?query={query}&filter={lic}&fields={fields}&page_size={page_size}&token={_key('FREESOUND_API_KEY')}"
    return http_get(url, expect_json=True)["results"]


def freesound_whoami():
    return http_get(f"https://freesound.org/apiv2/search/text/?query=test&page_size=1&fields=id&token={_key('FREESOUND_API_KEY')}", expect_json=True)["count"] >= 0


# ---- ElevenLabs: https://elevenlabs.io/docs/api-reference (xi-api-key header). Free plan: non-commercial and attribution; generating consumes credits.
def elevenlabs_whoami():
    return http_get("https://api.elevenlabs.io/v1/user/subscription", headers={"xi-api-key": _key("ELEVENLABS_API_KEY")}, expect_json=True)


def elevenlabs_sound_effect(text, out_path, duration_seconds=None, confirm_spend=False):
    if not confirm_spend:
        raise RuntimeError("refusing to spend ElevenLabs credits without confirm_spend=True (owner approval required)")
    import httpx
    body = {"text": text}
    if duration_seconds:
        body["duration_seconds"] = duration_seconds
    r = httpx.post("https://api.elevenlabs.io/v1/sound-generation", headers={"xi-api-key": _key("ELEVENLABS_API_KEY"), "User-Agent": UA}, json=body, timeout=120)
    r.raise_for_status()
    open(out_path, "wb").write(r.content)
    return out_path


# ---- Tripo AI: https://platform.tripo3d.ai/docs (Bearer key). Balance is free to read; every task costs credits.
def tripo_balance():
    return http_get("https://api.tripo3d.ai/v2/openapi/user/balance", headers={"Authorization": f"Bearer {_key('TRIPO_API_KEY')}"}, expect_json=True)


# ---- Meshy: https://docs.meshy.ai (Bearer key; API access needs a paid plan and prepaid credits: not usable on the free plan)
def meshy_balance():
    return http_get("https://api.meshy.ai/openapi/v1/balance", headers={"Authorization": f"Bearer {_key('MESHY_API_KEY')}"}, expect_json=True)


# ---- Sketchfab: https://sketchfab.com/developers/data-api/v3 (Token header). Search is public; download needs the account token and honours each model's licence.
def sketchfab_search(query, license_codes=("cc0", "by"), downloadable=True, count=24):
    lic = ",".join(license_codes)
    url = f"https://api.sketchfab.com/v3/search?type=models&q={query}&downloadable={'true' if downloadable else 'false'}&license={lic}&count={count}"
    return http_get(url, expect_json=True)["results"]


def sketchfab_whoami():
    return http_get("https://api.sketchfab.com/v3/me", headers={"Authorization": f"Token {_key('SKETCHFAB_API_TOKEN')}"}, expect_json=True)


# ---- Higgsfield: REST API with Authorization: Key <id>:<secret> (dashboard: cloud.higgsfield.ai), or the OAuth MCP server https://mcp.higgsfield.ai/mcp (no key; credits are deducted).
def higgsfield_headers():
    return {"Authorization": f"Key {_key('HIGGSFIELD_API_KEY_ID')}:{_key('HIGGSFIELD_API_KEY_SECRET')}"}


SERVICE_CHECKS = {"freesound": freesound_whoami, "elevenlabs": elevenlabs_whoami, "tripo": tripo_balance, "meshy": meshy_balance, "sketchfab": sketchfab_whoami}
