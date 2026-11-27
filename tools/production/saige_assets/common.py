import datetime, hashlib, json, os, re, sys, time
from pathlib import Path

from . import safety

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


def http_get(url, *, headers=None, timeout=60, retries=3, stream_to=None, expect_json=False, delay=0.0, allowed_hosts=None, max_bytes=None,
             expected_size=None, expected_md5=None, expected_sha256=None):
    """GET with bounded retries and a polite delay. Honors the standard CA environment variables (the agent proxy's CA bundle). Returns bytes / parsed
    JSON, or writes to `stream_to` (see `download`). `allowed_hosts`: HTTPS hosts the request and every redirect may reach (None: any)."""
    if stream_to is not None:
        return download(url, stream_to, headers=headers, timeout=timeout, retries=retries, delay=delay, allowed_hosts=allowed_hosts, max_bytes=max_bytes,
                        expected_size=expected_size, expected_md5=expected_md5, expected_sha256=expected_sha256)
    import httpx
    ca = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE") or True
    limit = max_bytes or 64 * 1024 ** 2
    last = None
    for attempt in range(retries):
        try:
            if delay:
                time.sleep(delay)
            if not safety.host_allowed(url, allowed_hosts):
                raise safety.UnsafePath(f"{url}: host not allowed")
            h = {"User-Agent": UA}
            h.update(headers or {})
            with httpx.stream("GET", url, headers=h, timeout=timeout, follow_redirects=True, verify=ca) as r:
                for hop in list(r.history) + [r]:
                    if not safety.host_allowed(hop.url, allowed_hosts):
                        raise safety.UnsafePath(f"{url}: redirected to {hop.url}, which is not an allowed host")
                r.raise_for_status()
                body = bytearray()
                for chunk in r.iter_bytes(1 << 20):
                    body += chunk
                    if len(body) > limit:
                        raise safety.IntegrityError(f"{url}: response larger than {limit} bytes")
            return json.loads(bytes(body)) if expect_json else bytes(body)
        except (safety.UnsafePath, safety.IntegrityError):
            raise
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"GET {url} failed: {last}")


def download(url, dest, *, headers=None, timeout=300, retries=3, delay=0.0, allowed_hosts=None, max_bytes=None, expected_size=None, expected_md5=None,
             expected_sha256=None):
    """Streams `url` to `dest` and returns "cached", "resumed" or "downloaded".
    * An existing `dest` is reused only if it matches what the source publishes (size, MD5, pinned SHA-256); with no expectation it is reused as
      before, and a mismatching one is downloaded again.
    * The body goes to `dest.part`; a `.part` left by an earlier attempt is resumed with a Range request only when the expected size is known and the
      server answers 206 with a Content-Range that starts where the file ends (otherwise it restarts from zero). The finished file is verified
      before it replaces `dest`; a mismatch removes it and raises IntegrityError (not retried: the source and the file disagree).
    * The response is refused beyond `max_bytes` (default 2 GiB), beyond a declared Content-Length, or at a redirect to a host outside `allowed_hosts`."""
    import httpx
    dest = Path(dest)
    exp = dict(expected_size=expected_size, expected_md5=expected_md5, expected_sha256=expected_sha256)
    if dest.exists():
        try:
            safety.verify(dest, **exp)
            return "cached"
        except safety.IntegrityError:
            dest.unlink()
    limit = max_bytes or safety.DEFAULT_MAX_BYTES
    if expected_size is not None and int(expected_size) > limit:
        raise safety.IntegrityError(f"{url}: {expected_size} bytes exceeds the limit of {limit}")
    if not safety.host_allowed(url, allowed_hosts):
        raise safety.UnsafePath(f"{url}: host not allowed")
    ca = os.environ.get("SSL_CERT_FILE") or os.environ.get("REQUESTS_CA_BUNDLE") or True
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(dest) + ".part")
    last, outcome = None, "downloaded"
    for attempt in range(retries):
        try:
            if delay:
                time.sleep(delay)
            h = {"User-Agent": UA}
            h.update(headers or {})
            start = tmp.stat().st_size if tmp.exists() else 0
            if start and expected_size is not None and start < int(expected_size):
                h["Range"] = f"bytes={start}-"
            else:
                start = 0
            with httpx.stream("GET", url, headers=h, timeout=timeout, follow_redirects=True, verify=ca) as r:
                for hop in list(r.history) + [r]:
                    if not safety.host_allowed(hop.url, allowed_hosts):
                        raise safety.UnsafePath(f"{url}: redirected to {hop.url}, which is not an allowed host")
                r.raise_for_status()
                mode = "wb"
                if start:
                    cr = r.headers.get("content-range", "")
                    if r.status_code == 206 and cr.startswith(f"bytes {start}-") and cr.endswith(f"/{int(expected_size)}"):
                        mode, outcome = "ab", "resumed"
                    else:
                        start = 0  # the server ignored or changed the range: start over
                declared = r.headers.get("content-length")
                if declared is not None and start + int(declared) > limit:
                    raise safety.IntegrityError(f"{url}: Content-Length {declared} exceeds the limit")
                n = start
                with open(tmp, mode) as f:
                    for chunk in r.iter_bytes(1 << 20):
                        n += len(chunk)
                        if n > limit:
                            raise safety.IntegrityError(f"{url}: more than {limit} bytes")
                        f.write(chunk)
                if declared is not None and n != start + int(declared):
                    raise RuntimeError(f"{url}: received {n - start} bytes, Content-Length said {declared}")
            try:
                safety.verify(tmp, **exp)
            except safety.IntegrityError:
                tmp.unlink(missing_ok=True)
                raise
            os.replace(tmp, dest)
            return outcome
        except (safety.UnsafePath, safety.IntegrityError):
            tmp.unlink(missing_ok=True)
            raise
        except Exception as e:  # noqa: BLE001  (network errors keep the .part for a resume)
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"GET {url} failed after {retries} attempts: {last}")


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
