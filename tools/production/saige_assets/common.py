import datetime, hashlib, json, os, re, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CATALOG_DIR = Path(os.environ.get("SAIGE_CATALOG_DIR", REPO / "assets" / "catalog"))
SCHEMA_PATH = REPO / "assets" / "schema" / "asset.schema.json"
TAXONOMY_PATH = REPO / "assets" / "taxonomy.json"
CACHE_DIR = Path(os.environ.get("SAIGE_ASSET_CACHE") or (Path.home() / ".cache" / "saige-assets"))
UA = "saige-assets/0.1 (+https://github.com/devincombs2007-rgb/SAIGE; asset pipeline)"


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_") or "asset"


def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def http_get(url, *, headers=None, timeout=60, retries=3, stream_to=None, expect_json=False, delay=0.0):
    """GET with retries and a polite delay. Honors the standard CA environment variables (the agent proxy's CA bundle). Returns bytes / parsed JSON, or writes to `stream_to`."""
    import httpx
    ca = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE") or True
    last = None
    for attempt in range(retries):
        try:
            if delay:
                time.sleep(delay)
            h = {"User-Agent": UA}
            h.update(headers or {})
            if stream_to is not None:
                Path(stream_to).parent.mkdir(parents=True, exist_ok=True)
                tmp = str(stream_to) + ".part"
                with httpx.stream("GET", url, headers=h, timeout=timeout, follow_redirects=True, verify=ca) as r:
                    r.raise_for_status()
                    with open(tmp, "wb") as f:
                        for chunk in r.iter_bytes(1 << 20):
                            f.write(chunk)
                os.replace(tmp, stream_to)
                return None
            r = httpx.get(url, headers=h, timeout=timeout, follow_redirects=True, verify=ca)
            r.raise_for_status()
            return r.json() if expect_json else r.content
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"GET {url} failed: {last}")


def load_json(path):
    with open(path, encoding="utf8") as f:
        return json.load(f)


def dump_json(path, obj):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf8") as f:
        json.dump(obj, f, indent=1, sort_keys=True, ensure_ascii=False)
        f.write("\n")


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
