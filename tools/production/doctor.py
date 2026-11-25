#!/usr/bin/env python3
"""Reports what the SAIGE production toolchain can do in this environment: tools and versions, Python and Node modules, which API secrets are present (names only,
never values), and which service hosts the network policy lets this session reach. Makes no paid or credit-consuming request: reachability is a plain HTTPS request to the
host's front page. Exit code 0 always (it is a report); `--json` prints machine-readable output; `--no-network` skips connectivity.

  doctor.py [--json] [--no-network]
"""
import json, os, shutil, subprocess, sys, importlib, urllib.request, urllib.error, ssl

TOOLS = [  # name, version command, what it is for
    ("ffmpeg", ["ffmpeg", "-version"], "audio/video transcode, loudnorm, capture encode"),
    ("sox", ["sox", "--version"], "audio effects, trimming, analysis"),
    ("convert", ["convert", "-version"], "ImageMagick: image conversion and montage"),
    ("vips", ["vips", "--version"], "libvips: fast large-image resize and conversion"),
    ("blender", ["blender", "--version"], "headless 3D conversion, LODs, collision hulls, retargeting"),
    ("gltf-transform", ["gltf-transform", "--version"], "glTF optimisation (dedup, prune, weld, simplify, compress)"),
    ("gltfpack", ["gltfpack", "-h"], "meshoptimizer's glTF packer (quantise, simplify, meshopt compression)"),
    ("assimp", ["assimp", "version"], "format conversion (FBX/OBJ/DAE to glTF) from the command line"),
    ("glslangValidator", ["glslangValidator", "--version"], "HLSL/GLSL to SPIR-V for the shader pipeline"),
    ("opusenc", ["opusenc", "--version"], "Opus encoder"),
    ("oggenc", ["oggenc", "--version"], "Vorbis encoder"),
    ("flac", ["flac", "--version"], "FLAC encoder"),
    ("lame", ["lame", "--version"], "MP3 encoder"),
    ("optipng", ["optipng", "-v"], "lossless PNG optimisation"),
    ("pngquant", ["pngquant", "--version"], "lossy PNG palette quantisation"),
    ("cwebp", ["cwebp", "-version"], "WebP encoder"),
    ("git-lfs", ["git-lfs", "version"], "large file storage (private asset repository option)"),
    ("node", ["node", "--version"], "Node runtime for the glTF tools"),
    ("python3", ["python3", "--version"], "Python runtime"),
    ("cmake", ["cmake", "--version"], "engine build"),
    ("ninja", ["ninja", "--version"], "engine build"),
]
PY_MODULES = ["numpy", "scipy", "PIL", "cv2", "imageio", "trimesh", "pygltflib", "networkx", "fast_simplification", "xatlas", "soundfile", "librosa", "pyloudnorm", "mutagen", "httpx", "yaml", "jsonschema", "tqdm"]
NODE_MODULES = ["@gltf-transform/core", "@gltf-transform/functions", "@gltf-transform/extensions", "meshoptimizer", "gltfpack", "ktx2-encoder", "sharp", "draco3dgltf", "gltf-validator"]
SECRETS = [  # env var, service, what it unlocks
    ("FREESOUND_API_KEY", "Freesound", "search and preview download (token auth)"),
    ("FREESOUND_CLIENT_ID", "Freesound", "OAuth2 client for original-quality download"),
    ("ELEVENLABS_API_KEY", "ElevenLabs", "sound effects, voices, dialogue"),
    ("TRIPO_API_KEY", "Tripo AI", "text/image to 3D"),
    ("MESHY_API_KEY", "Meshy AI", "text/image to 3D, texturing (paid plans only)"),
    ("SKETCHFAB_API_TOKEN", "Sketchfab", "search and download of CC models"),
    ("HIGGSFIELD_API_KEY_ID", "Higgsfield", "concept art and video generation API"),
]
HOSTS = [  # host, service, auth needed beyond the network allowance
    ("polyhaven.com", "Poly Haven", "none (CC0)"), ("api.polyhaven.com", "Poly Haven API", "none"), ("dl.polyhaven.org", "Poly Haven files", "none"),
    ("ambientcg.com", "ambientCG", "none (CC0)"), ("docs.ambientcg.com", "ambientCG docs", "none"), ("raw.githubusercontent.com", "Khronos sample assets, raw files", "none"),
    ("registry.npmjs.org", "npm", "none"), ("pypi.org", "PyPI", "none"), ("archive.ubuntu.com", "apt", "none"),
    ("kenney.nl", "Kenney", "none (CC0)"), ("quaternius.com", "Quaternius", "none (CC0)"), ("opengameart.org", "OpenGameArt", "none (mixed licences)"),
    ("freesound.org", "Freesound", "API key / OAuth2"), ("api.elevenlabs.io", "ElevenLabs", "API key"), ("api.tripo3d.ai", "Tripo AI", "API key"),
    ("api.meshy.ai", "Meshy", "API key (paid)"), ("api.sketchfab.com", "Sketchfab", "API token"), ("www.mixamo.com", "Mixamo", "Adobe login, browser only"),
    ("sonniss.com", "Sonniss GameAudioGDC", "manual download"), ("pixabay.com", "Pixabay audio", "manual / API key"), ("www.cgtrader.com", "CGTrader", "login, per-model licence"),
    ("download.blender.org", "Blender releases", "none"), ("huggingface.co", "Hugging Face (open 3D models)", "none / token"), ("github.com", "GitHub releases (KTX-Software, MakeHuman)", "none"),
]


def run(cmd):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        text = (out.stdout or out.stderr).strip().splitlines()
        return text[0] if text else "ok"
    except Exception:
        return None


def check_tools(path_extra):
    res = []
    for name, cmd, purpose in TOOLS:
        exe = shutil.which(cmd[0], path=os.pathsep.join([path_extra, os.environ.get("PATH", "")]))
        ver = run([exe] + cmd[1:]) if exe else None
        res.append({"name": name, "found": exe is not None, "path": exe, "version": ver, "purpose": purpose})
    return res


def check_py():
    py = os.path.join(os.environ.get("SAIGE_TOOLS_HOME", os.path.expanduser("~/.saige-tools")), "venv", "bin", "python")
    interp = py if os.path.exists(py) else sys.executable
    code = "import importlib,json;r={}\nfor m in %r:\n    try:\n        mod=importlib.import_module(m);r[m]=getattr(mod,'__version__','ok')\n    except Exception as e:\n        r[m]=None\nprint(json.dumps(r))" % (PY_MODULES,)
    try:
        out = subprocess.run([interp, "-W", "ignore", "-c", code], capture_output=True, text=True, timeout=120)
        return interp, json.loads(out.stdout.strip().splitlines()[-1])
    except Exception:
        return interp, {m: None for m in PY_MODULES}


def check_node():
    base = os.environ.get("SAIGE_TOOLS_HOME", os.path.expanduser("~/.saige-tools")) + "/node/node_modules"
    res = {}
    for m in NODE_MODULES:
        pj = os.path.join(base, m, "package.json")
        if os.path.exists(pj):
            res[m] = json.load(open(pj)).get("version")
        else:
            res[m] = None
    return base, res


def reach(host):
    ctx = ssl.create_default_context()
    ca = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE")
    if ca and os.path.exists(ca):
        ctx = ssl.create_default_context(cafile=ca)
    req = urllib.request.Request("https://" + host + "/", headers={"User-Agent": "saige-doctor"}, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=12, context=ctx) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code  # any HTTP answer means the host is reachable (a 403 from the origin counts; a 403 from the proxy is told apart below)
    except Exception as e:
        text = str(getattr(e, "reason", e))
        return "BLOCKED" if "Tunnel connection failed: 403" in text or "no rule" in text else "error: " + text[:40]


def main():
    as_json = "--json" in sys.argv
    tools_home = os.environ.get("SAIGE_TOOLS_HOME", os.path.expanduser("~/.saige-tools"))
    extra = os.pathsep.join([tools_home + "/node/node_modules/.bin", tools_home + "/venv/bin"])
    report = {"tools": check_tools(extra)}
    interp, mods = check_py()
    report["python"] = {"interpreter": interp, "modules": mods}
    nbase, nmods = check_node()
    report["node"] = {"base": nbase, "modules": nmods}
    report["secrets"] = [{"env": e, "service": s, "unlocks": u, "present": bool(os.environ.get(e))} for e, s, u in SECRETS]
    if "--no-network" not in sys.argv:
        hosts = []
        for h, s, a in HOSTS:
            code = reach(h)
            hosts.append({"host": h, "service": s, "auth": a, "status": code, "reachable": isinstance(code, int)})
        report["hosts"] = hosts
    if as_json:
        print(json.dumps(report, indent=1))
        return
    print("== tools")
    for t in report["tools"]:
        print(f"  {'ok ' if t['found'] else '-- '} {t['name']:<18} {t['version'] or ''}")
    print("== python (%s)" % interp)
    print("  " + ", ".join(f"{m}{'' if v else ' (MISSING)'}" for m, v in mods.items()))
    print("== node modules (%s)" % nbase)
    print("  " + ", ".join(f"{m}{'' if v else ' (MISSING)'}" for m, v in nmods.items()))
    print("== secrets present (names only)")
    for s in report["secrets"]:
        print(f"  {'set    ' if s['present'] else 'not set'} {s['env']:<28} {s['service']}")
    if "hosts" in report:
        print("== network (HTTP status of the front page; BLOCKED = the agent proxy's policy does not allow the host)")
        for h in report["hosts"]:
            print(f"  {str(h['status']):<22} {h['host']:<28} {h['service']}  [{h['auth']}]")


main()
