"""Original procedural PBR texture sets for Showcase 01. Pure numpy and the standard library; no third-party image or asset.

Every set is tileable (all noise is periodic) and is written as three PNGs:
  <name>_albedo.png   sRGB base color
  <name>_normal.png   tangent-space normal, +Y up in the image (glTF), linear
  <name>_orm.png      R occlusion, G roughness, B metallic, linear (glTF packing)
Deterministic for a given numpy version (fixed seeds).
"""
import struct
import zlib

import numpy as np

SIZE = 512


def write_png(path, array):
    """Write an 8-bit grayscale/RGB/RGBA PNG with the standard library (zlib)."""
    a = np.ascontiguousarray(array, dtype=np.uint8)
    h, w = a.shape[:2]
    channels = 1 if a.ndim == 2 else a.shape[2]
    color_type = {1: 0, 3: 2, 4: 6}[channels]
    raw = b"".join(b"\x00" + a[y].tobytes() for y in range(h))  # filter type 0 per row

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def to_u8(x):
    return np.clip(np.rint(x * 255.0), 0, 255).astype(np.uint8)


def srgb_encode(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(linear, 1 / 2.4) - 0.055)


def srgb_decode(srgb):
    srgb = np.clip(srgb, 0.0, 1.0)
    return np.where(srgb <= 0.04045, srgb / 12.92, np.power((srgb + 0.055) / 1.055, 2.4))


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def fractal_noise(rng, size=SIZE, beta=2.0, low=1, high=None):
    """Tileable noise: random spectrum shaped by 1/f^(beta/2), normalized to [0, 1]."""
    high = high or size // 2
    spectrum = rng.normal(size=(size, size // 2 + 1)) + 1j * rng.normal(size=(size, size // 2 + 1))
    fy = np.fft.fftfreq(size)[:, None] * size
    fx = np.fft.rfftfreq(size)[None, :] * size
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1.0
    amp = 1.0 / np.power(f, beta / 2.0)
    amp[(f < low) | (f > high)] = 0.0
    n = np.fft.irfft2(spectrum * amp, s=(size, size))
    n -= n.min()
    return n / max(n.max(), 1e-9)


def normal_from_height(height, strength):
    """Tangent-space normal map (glTF: +Y is up in the image, so the image's up is -row direction). Periodic gradients."""
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, 1, axis=0) - np.roll(height, -1, axis=0)) * 0.5  # up in the image is a smaller row index
    nx, ny, nz = -dx * strength, -dy * strength, np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    n = np.stack([nx / length, ny / length, nz / length], axis=-1)
    return n * 0.5 + 0.5


def voronoi(rng, cells, size=SIZE, jitter=0.85):
    """Periodic Voronoi: returns (distance to the nearest point, distance to the second nearest, nearest cell index)."""
    grid = np.stack(np.meshgrid(np.arange(cells), np.arange(cells), indexing="ij"), -1).reshape(-1, 2).astype(np.float64)
    pts = (grid + 0.5 + (rng.random(grid.shape) - 0.5) * jitter) / cells
    ys, xs = np.mgrid[0:size, 0:size]
    p = np.stack([xs, ys], -1).reshape(-1, 2) / size
    best1 = np.full(len(p), 9.0)
    best2 = np.full(len(p), 9.0)
    index = np.zeros(len(p), dtype=np.int64)
    for i, c in enumerate(pts):
        d = p - c
        d -= np.rint(d)  # wrap: the texture tiles
        dist = np.sqrt((d * d).sum(-1))
        closer = dist < best1
        best2 = np.where(closer, best1, np.minimum(best2, dist))
        index = np.where(closer, i, index)
        best1 = np.where(closer, dist, best1)
    shape = (size, size)
    return best1.reshape(shape), best2.reshape(shape), index.reshape(shape)


def pack(albedo_linear, height, roughness, metallic, ao, normal_strength):
    albedo = to_u8(srgb_encode(albedo_linear))
    normal = to_u8(normal_from_height(height, normal_strength))
    orm = to_u8(np.stack([ao, roughness, metallic], axis=-1))
    return albedo, normal, orm


def cobble(seed=11):
    rng = np.random.default_rng(seed)
    d1, d2, cell = voronoi(rng, 7)
    edge = (d2 - d1) * 7.0  # roughly 0 at the seams between stones
    stone = smoothstep(0.02, 0.16, edge)
    grain = fractal_noise(rng, beta=1.6, low=4, high=140)
    pits = fractal_noise(rng, beta=1.0, low=20, high=200)
    height = stone * (0.65 + 0.25 * grain) + 0.08 * pits * stone
    tone = (rng.random(49)[cell % 49] - 0.5)
    base = np.stack([0.34 + 0.12 * tone, 0.31 + 0.11 * tone, 0.27 + 0.09 * tone], -1)
    albedo = base * (0.75 + 0.5 * grain[..., None]) * (0.55 + 0.45 * stone[..., None])
    albedo[..., 1] *= 0.98
    mortar = np.array([0.10, 0.09, 0.08])
    albedo = albedo * stone[..., None] + mortar * (1 - stone[..., None]) * (0.8 + 0.4 * pits[..., None])
    roughness = np.clip(0.72 + 0.22 * (grain - 0.5) + 0.2 * (1 - stone), 0, 1)
    ao = np.clip(0.35 + 0.65 * smoothstep(0.0, 0.22, edge), 0, 1)
    return pack(albedo, height, roughness, np.zeros_like(roughness), ao, 6.0)


def brick(seed=23):
    rng = np.random.default_rng(seed)
    rows, cols = 16, 4
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    row_h, col_w = SIZE / rows, SIZE / cols
    row = np.floor(ys / row_h).astype(int)
    shifted = xs + (row % 2) * col_w * 0.5
    col = np.floor(shifted / col_w).astype(int) % cols
    fy = (ys / row_h) - np.floor(ys / row_h)
    fx = (shifted / col_w) - np.floor(shifted / col_w)
    mortar_w = 0.07
    inside_y = smoothstep(0.0, mortar_w, fy) * (1 - smoothstep(1 - mortar_w, 1.0, fy))
    inside_x = smoothstep(0.0, mortar_w / 2, fx) * (1 - smoothstep(1 - mortar_w / 2, 1.0, fx))
    body = inside_x * inside_y
    grain = fractal_noise(rng, beta=1.4, low=6, high=160)
    pits = fractal_noise(rng, beta=0.9, low=30, high=220)
    per_brick = rng.random((rows, cols))[row % rows, col]
    base = np.stack([0.40 + 0.16 * per_brick, 0.15 + 0.06 * per_brick, 0.10 + 0.03 * per_brick], -1)
    albedo = base * (0.7 + 0.6 * grain[..., None])
    mortar_color = np.array([0.30, 0.28, 0.25]) * (0.8 + 0.4 * pits[..., None])
    albedo = albedo * body[..., None] + mortar_color * (1 - body[..., None])
    height = body * (0.7 + 0.2 * grain) - 0.1 * pits * (1 - body) + 0.06 * pits * body
    roughness = np.clip(0.80 + 0.15 * (pits - 0.5) + 0.1 * (1 - body), 0, 1)
    ao = np.clip(0.4 + 0.6 * body, 0, 1)
    return pack(albedo, height, roughness, np.zeros_like(roughness), ao, 7.0)


def wood(seed=37):
    rng = np.random.default_rng(seed)
    planks = 6
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    plank = np.floor(xs / (SIZE / planks)).astype(int)
    fx = xs / (SIZE / planks) - plank
    warp = fractal_noise(rng, beta=2.2, low=1, high=24)
    fine = fractal_noise(rng, beta=1.2, low=40, high=250)
    per_plank = rng.random(planks)[plank]
    rings = 0.5 + 0.5 * np.sin((fx * 18.0 + warp * 14.0 + per_plank * 40.0 + ys / SIZE * 3.0) * 2.0 * np.pi / 2.0)
    streak = fractal_noise(rng, beta=2.0, low=2, high=60)
    lo, hi = np.array([0.20, 0.11, 0.05]), np.array([0.40, 0.23, 0.11])
    t = np.clip(0.55 * rings + 0.45 * streak, 0, 1)[..., None]
    albedo = (lo * (1 - t) + hi * t) * (0.8 + 0.4 * fine[..., None]) * (0.85 + 0.3 * per_plank[..., None])
    seam = smoothstep(0.0, 0.05, fx) * (1 - smoothstep(0.95, 1.0, fx))
    albedo = albedo * (0.25 + 0.75 * seam[..., None])
    height = 0.5 * seam + 0.18 * rings + 0.05 * fine
    roughness = np.clip(0.55 + 0.25 * (fine - 0.5) + 0.2 * (1 - seam), 0, 1)
    ao = np.clip(0.35 + 0.65 * seam, 0, 1)
    return pack(albedo, height, roughness, np.zeros_like(roughness), ao, 3.0)


def metal(seed=51):
    rng = np.random.default_rng(seed)
    rust_field = fractal_noise(rng, beta=2.0, low=2, high=70)
    rust_detail = fractal_noise(rng, beta=1.4, low=10, high=200)
    rust = smoothstep(0.52, 0.64, rust_field + 0.3 * (rust_detail - 0.5))
    scratch_src = fractal_noise(rng, beta=0.6, low=60, high=250)
    # Brushed scratches: blur the noise along one axis (periodic).
    kernel = np.zeros(SIZE)
    kernel[:40] = 1.0 / 40.0
    scratches = np.real(np.fft.ifft(np.fft.fft(scratch_src, axis=1) * np.fft.fft(kernel)[None, :], axis=1))
    scratches = (scratches - scratches.min()) / (scratches.max() - scratches.min())
    steel = np.array([0.55, 0.56, 0.58]) * (0.8 + 0.4 * scratches[..., None])
    rust_color = np.stack([0.38 + 0.12 * rust_detail, 0.16 + 0.07 * rust_detail, 0.07 + 0.03 * rust_detail], -1)
    albedo = steel * (1 - rust[..., None]) + rust_color * rust[..., None]
    height = 0.6 * scratches + 0.3 * rust * rust_detail + 0.2 * rust_field
    metallic = np.clip(1.0 - rust * 0.92, 0, 1)
    roughness = np.clip(0.28 + 0.12 * (scratches - 0.5) + 0.58 * rust, 0, 1)
    ao = np.clip(0.55 + 0.45 * (1 - rust * rust_detail), 0, 1)
    return pack(albedo, height, roughness, metallic, ao, 4.0)


def marble(seed=71):
    rng = np.random.default_rng(seed)
    flow = fractal_noise(rng, beta=3.0, low=1, high=10)
    detail = fractal_noise(rng, beta=1.6, low=6, high=80)
    ridge = 1.0 - np.abs(2.0 * np.clip(flow + 0.06 * (detail - 0.5), 0, 1) - 1.0)
    vein_mask = np.power(ridge, 18.0) * (0.55 + 0.45 * detail)
    cloud = fractal_noise(rng, beta=2.2, low=1, high=30)
    base = np.array([0.78, 0.76, 0.72]) * (0.88 + 0.22 * cloud[..., None])
    vein_color = np.array([0.22, 0.24, 0.30])
    albedo = base * (1 - 0.8 * vein_mask[..., None]) + vein_color * 0.8 * vein_mask[..., None]
    fine = fractal_noise(rng, beta=1.0, low=30, high=240)
    height = 0.15 * cloud + 0.06 * fine - 0.25 * vein_mask
    roughness = np.clip(0.20 + 0.20 * (fine - 0.5) + 0.25 * vein_mask, 0, 1)
    ao = np.clip(0.9 + 0.1 * cloud, 0, 1)
    return pack(albedo, height, roughness, np.zeros_like(roughness), ao, 1.5)


def blob_shadow(size=128):
    """Radial soft shadow decal: black with an alpha falloff. Linear and sRGB agree for black."""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64)
    r = np.sqrt(((xs + 0.5) / size - 0.5) ** 2 + ((ys + 0.5) / size - 0.5) ** 2) * 2.0
    alpha = np.power(1.0 - smoothstep(0.42, 1.0, r), 1.2) * 0.92
    rgba = np.zeros((size, size, 4))
    rgba[..., 3] = alpha
    return to_u8(rgba)


GENERATORS = {"cobble": cobble, "brick": brick, "wood": wood, "metal": metal, "marble": marble}


def generate_all(out_dir):
    import os
    os.makedirs(out_dir, exist_ok=True)
    for name, fn in GENERATORS.items():
        albedo, normal, orm = fn()
        write_png(os.path.join(out_dir, f"{name}_albedo.png"), albedo)
        write_png(os.path.join(out_dir, f"{name}_normal.png"), normal)
        write_png(os.path.join(out_dir, f"{name}_orm.png"), orm)
    write_png(os.path.join(out_dir, "blob_shadow.png"), blob_shadow())
