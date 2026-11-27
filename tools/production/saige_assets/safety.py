"""Safety rules for acquisition (added 2026-11-27 after the review of Gemini's Asset Acquisition 001 on #43, comment 7209985020, which found the same
classes of defect in this tool): every path written comes from a source and is checked to stay inside its directory; archives are unpacked member by
member with limits instead of ZipFile.extractall; downloads are bounded, verified against what the source publishes, and only follow redirects to the
hosts a source names."""
import hashlib
import os
import stat
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

DEFAULT_MAX_BYTES = 2 * 1024 ** 3          # one file
MAX_ARCHIVE_MEMBERS = 5000
MAX_ARCHIVE_TOTAL_BYTES = 8 * 1024 ** 3   # all members of one archive, uncompressed
MAX_COMPRESSION_RATIO = 250               # per member; a zip bomb compresses far better than texture maps
REQUIRE_HTTPS = True                      # tests against a local server turn this off


class UnsafePath(ValueError):
    pass


class IntegrityError(RuntimeError):
    pass


def contained(base, rel):
    """The path `rel` (from a source: an API key, a ZIP member name) under `base`, or UnsafePath. Absolute paths, drive letters, backslashes, `..`
    components and anything that resolves outside `base` (through a symlink too) are refused."""
    if not isinstance(rel, str) or not rel or "\x00" in rel or "\\" in rel:
        raise UnsafePath(f"unsafe path {rel!r}")
    p = PurePosixPath(rel)
    if p.is_absolute() or any(part in ("..", "") for part in p.parts) or (p.parts and ":" in p.parts[0]):
        raise UnsafePath(f"unsafe path {rel!r}")
    base_r = Path(base).resolve()
    out = (base_r / Path(*p.parts)).resolve()
    if out != base_r and base_r not in out.parents:
        raise UnsafePath(f"{rel!r} leaves {base}")
    return out


def host_allowed(url, allowed_hosts):
    if allowed_hosts is None:
        return True
    host = (urlsplit(str(url)).hostname or "").lower()
    return (urlsplit(str(url)).scheme == "https" or not REQUIRE_HTTPS) and any(host == h or host.endswith("." + h) for h in allowed_hosts)


def safe_extract(zip_path, dest, *, max_members=MAX_ARCHIVE_MEMBERS, max_total=MAX_ARCHIVE_TOTAL_BYTES, max_ratio=MAX_COMPRESSION_RATIO):
    """Unpacks `zip_path` into `dest` member by member and returns the written paths. Refuses (before writing anything) an archive with too many
    members, a member path outside `dest`, a symbolic link or special file, an uncompressed total above `max_total` or a member that compresses
    better than `max_ratio`; then streams each member and stops if it grows beyond its declared size."""
    dest = Path(dest)
    with zipfile.ZipFile(zip_path) as z:
        infos = z.infolist()
        if len(infos) > max_members:
            raise UnsafePath(f"{zip_path}: {len(infos)} members (limit {max_members})")
        plan, total = [], 0
        for info in infos:
            mode = (info.external_attr >> 16) & 0xFFFF
            if mode and (stat.S_ISLNK(mode) or not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))):
                raise UnsafePath(f"{zip_path}: {info.filename!r} is not a regular file")
            target = contained(dest, info.filename.rstrip("/")) if info.filename.rstrip("/") else None
            if info.is_dir():
                continue
            total += info.file_size
            if total > max_total:
                raise UnsafePath(f"{zip_path}: more than {max_total} bytes uncompressed")
            if info.file_size > 1 << 20 and info.compress_size and info.file_size / info.compress_size > max_ratio:
                raise UnsafePath(f"{zip_path}: {info.filename!r} compresses {info.file_size // max(info.compress_size, 1)}:1")
            plan.append((info, target))
        written = []
        for info, target in plan:
            target.parent.mkdir(parents=True, exist_ok=True)
            n = 0
            with z.open(info) as src, open(target, "wb") as out:
                for block in iter(lambda: src.read(1 << 20), b""):
                    n += len(block)
                    if n > info.file_size:
                        raise UnsafePath(f"{zip_path}: {info.filename!r} is larger than declared")
                    out.write(block)
            written.append(target)
        return written


def file_digest(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify(path, *, expected_size=None, expected_md5=None, expected_sha256=None):
    """IntegrityError unless the file matches every expectation given."""
    size = os.path.getsize(path)
    if expected_size is not None and size != int(expected_size):
        raise IntegrityError(f"{path}: {size} bytes, the source says {expected_size}")
    if expected_md5 and file_digest(path, "md5") != expected_md5.lower():
        raise IntegrityError(f"{path}: MD5 differs from the source's")
    if expected_sha256 and file_digest(path, "sha256") != expected_sha256.lower():
        raise IntegrityError(f"{path}: SHA-256 differs from the pinned value")


def has_expectation(expected_size=None, expected_md5=None, expected_sha256=None):
    return expected_size is not None or bool(expected_md5) or bool(expected_sha256)


def missing_gltf_dependencies(gltf_path):
    """The external `uri`s (buffers and images) a .gltf references that do not exist as files inside its directory; data: URIs are embedded and
    always present. A path that leaves the directory is reported as missing. Empty for a complete asset (and for a .glb, which embeds its buffer)."""
    import json
    from urllib.parse import unquote
    gltf_path = Path(gltf_path)
    if gltf_path.suffix.lower() != ".gltf":
        return []
    js = json.loads(gltf_path.read_text(encoding="utf8"))
    missing = []
    for item in list(js.get("buffers", [])) + list(js.get("images", [])):
        uri = item.get("uri")
        if not uri or uri.startswith("data:"):
            continue
        try:
            target = contained(gltf_path.parent, unquote(uri))
        except UnsafePath:
            missing.append(uri)
            continue
        if not target.is_file():
            missing.append(uri)
    return missing
