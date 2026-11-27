import hashlib
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple


USER_AGENT = "SAIGE-MasterAssetAcquisition/1.0 (+https://github.com/devincombs2007-rgb/SAIGE)"


class DownloadError(RuntimeError):
    pass


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


class ResilientDownloader:
    """HTTP downloader with retry, exponential backoff, Range resume, and SHA-256 verification."""

    def __init__(self, timeout_sec: float = 30.0, max_retries: int = 4, backoff_base: float = 1.5):
        self.timeout_sec = timeout_sec
        self.max_retries = max_retries
        self.backoff_base = backoff_base

    def fetch_json(self, url: str) -> Any:
        last_err: Optional[Exception] = None
        for attempt in range(self.max_retries):
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=self.timeout_sec) as resp:
                    return json.loads(resp.read().decode("utf-8"))
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                last_err = exc
                if attempt + 1 < self.max_retries:
                    time.sleep(self.backoff_base ** attempt)
        raise DownloadError(f"Failed JSON GET {url}: {last_err}")

    def download_file(
        self,
        url: str,
        dest_path: str,
        expected_sha256: Optional[str] = None,
    ) -> Tuple[int, str]:
        os.makedirs(os.path.dirname(os.path.abspath(dest_path)), exist_ok=True)
        part_path = dest_path + ".part"

        if os.path.exists(dest_path):
            actual = sha256_file(dest_path)
            if expected_sha256 is None or actual.lower() == expected_sha256.lower():
                return os.path.getsize(dest_path), actual

        last_err: Optional[Exception] = None
        for attempt in range(self.max_retries):
            existing_bytes = os.path.getsize(part_path) if os.path.exists(part_path) else 0
            headers = {"User-Agent": USER_AGENT}
            if existing_bytes > 0:
                headers["Range"] = f"bytes={existing_bytes}-"
            req = urllib.request.Request(url, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=self.timeout_sec) as resp:
                    status = getattr(resp, "status", 200)
                    mode = "ab" if (existing_bytes > 0 and status == 206) else "wb"
                    with open(part_path, mode) as out:
                        while True:
                            buf = resp.read(256 * 1024)
                            if not buf:
                                break
                            out.write(buf)
                os.replace(part_path, dest_path)
                actual = sha256_file(dest_path)
                if expected_sha256 and actual.lower() != expected_sha256.lower():
                    os.remove(dest_path)
                    raise DownloadError(
                        f"SHA-256 mismatch for {url}: expected {expected_sha256}, got {actual}"
                    )
                return os.path.getsize(dest_path), actual
            except (urllib.error.URLError, TimeoutError, OSError, DownloadError) as exc:
                last_err = exc
                if attempt + 1 < self.max_retries:
                    time.sleep(self.backoff_base ** attempt)
        raise DownloadError(f"Failed downloading {url}: {last_err}")


class PolyHavenClient:
    """Official Poly Haven public API client (https://api.polyhaven.com, CC0-1.0)."""

    BASE_URL = "https://api.polyhaven.com"

    def __init__(self, downloader: ResilientDownloader, base_url: str = BASE_URL):
        self.dl = downloader
        self.base_url = base_url.rstrip("/")

    def list_assets(self, asset_type: Optional[str] = None) -> Dict[str, Any]:
        q = f"?type={urllib.parse.quote(asset_type)}" if asset_type else ""
        return self.dl.fetch_json(f"{self.base_url}/assets{q}")

    def get_files(self, slug: str) -> Dict[str, Any]:
        return self.dl.fetch_json(f"{self.base_url}/files/{urllib.parse.quote(slug)}")

    def resolve_download_url(
        self, slug: str, kind: str, resolution: str = "2k", fmt: str = "gltf"
    ) -> Optional[str]:
        files = self.get_files(slug)
        if kind == "hdri":
            block = files.get("hdri", {}).get(resolution, {})
            entry = block.get(fmt) or block.get("exr") or block.get("hdr")
            return entry.get("url") if isinstance(entry, dict) else None
        if kind == "model":
            block = files.get("gltf", {}).get(resolution, {}) or files.get("fbx", {}).get(resolution, {})
            entry = block.get(fmt) or block.get("gltf") or block.get("fbx")
            return entry.get("url") if isinstance(entry, dict) else None
        # textures
        for map_key in ("Diffuse", "nor_gl", "Rough", "AO", "arm"):
            block = files.get(map_key, {}).get(resolution, {})
            entry = block.get("png") or block.get("jpg")
            if isinstance(entry, dict) and entry.get("url"):
                return entry["url"]
        return None


class AmbientCGClient:
    """Official ambientCG v2 JSON API client (https://ambientcg.com/api/v2/full_json, CC0-1.0)."""

    BASE_URL = "https://ambientcg.com/api/v2/full_json"

    def __init__(self, downloader: ResilientDownloader, base_url: str = BASE_URL):
        self.dl = downloader
        self.base_url = base_url

    def query_materials(self, query: str = "", limit: int = 20, offset: int = 0) -> List[Dict[str, Any]]:
        params = urllib.parse.urlencode(
            {
                "type": "Material",
                "q": query,
                "limit": limit,
                "offset": offset,
                "include": "downloadData,previewData,tagData",
            }
        )
        data = self.dl.fetch_json(f"{self.base_url}?{params}")
        return data.get("foundAssets", [])
