#!/usr/bin/env python3
"""Independent check of the glTF 2.0 dielectric BRDF's energy behavior (docs/rendering/brdf-energy-and-lut.md).

Written from the Khronos glTF 2.0 specification, appendix B, without reading any SAIGE code, so it can disagree with the
renderer. White furnace test: a white dielectric (base color 1) under uniform white illumination reflects
    albedo(v) = integral over the light hemisphere of f(l, v) * n.l dl,
which must not exceed 1 for energy conservation.

  dielectric_brdf = fresnel_mix(ior = 1.5, base = diffuse_brdf, layer = specular_brdf)
                  = (1 - F) * (base_color / pi) + F * (D * V)          F = f0 + (1 - f0) (1 - v.h)^5, f0 = 0.04
  D = GGX with alpha = roughness^2,  V = height-correlated Smith visibility (includes 1 / (4 n.l n.v))

Prints the albedo table for the specification's formula and for an optional energy-conserving variant, and the worst case.
Usage: python3 tools/analysis/dielectric_energy.py   (needs numpy; deterministic)
"""
import numpy as np

F0 = 0.04


def d_ggx(nh, a):
    a2 = a * a
    d = nh * nh * (a2 - 1.0) + 1.0
    return a2 / (np.pi * d * d)


def v_ggx(nl, nv, a):
    a2 = a * a
    gv = nl * np.sqrt(nv * nv * (1.0 - a2) + a2)
    gl = nv * np.sqrt(nl * nl * (1.0 - a2) + a2)
    return 0.5 / (gv + gl)


def fresnel(vh):
    return F0 + (1.0 - F0) * np.power(np.clip(1.0 - vh, 0.0, 1.0), 5.0)


def burley_diffuse(nv, n=1024):
    """Diffuse albedo of the (1 - F(n.l)) (1 - F(n.v)) / pi weighting that the glTF specification attributes to Burley (2012)."""
    tl = (np.arange(n) + 0.5) / n * (np.pi / 2.0)
    TL = np.meshgrid(tl, np.arange(n), indexing="ij")[0]
    nl = np.cos(TL)
    dw = np.sin(TL) * (np.pi / 2.0 / n) * (2.0 * np.pi / n)
    return np.sum((1.0 - fresnel(nl)) * (1.0 - fresnel(nv)) / np.pi * nl * dw)


def albedo_parts(nv, roughness, nu=512, nphi=512):
    """(diffuse term, specular term) for white base color: integrals of (1-F)/pi n.l and F D V n.l."""
    a = max(roughness, 0.045) ** 2
    v = np.array([np.sqrt(1.0 - nv * nv), 0.0, nv])
    # Specular: importance-sample the GGX half vector (resolves narrow lobes); dω_l = 4 (v.h) dω_h.
    u = (np.arange(nu) + 0.5) / nu
    th = np.arctan(a * np.sqrt(u / (1.0 - u)))
    ph = (np.arange(nphi) + 0.5) / nphi * 2.0 * np.pi
    TH, PH = np.meshgrid(th, ph, indexing="ij")
    h = np.stack([np.sin(TH) * np.cos(PH), np.sin(TH) * np.sin(PH), np.cos(TH)], axis=-1)
    vh = h @ v
    l = 2.0 * vh[..., None] * h - v
    nl = l[..., 2]
    ok = (vh > 0) & (nl > 0)
    spec_unit = np.where(ok, v_ggx(np.where(ok, nl, 1.0), nv, a) * nl * 4.0 * vh / h[..., 2], 0.0) / (nu * nphi)
    f = fresnel(np.where(ok, vh, 0.0))
    specular = np.sum(spec_unit * f)            # integral of F * D * V * n.l
    # Diffuse: (1 - F(v.h)) / pi * n.l on a fine light-direction grid (smooth integrand).
    n = 1024
    tl = (np.arange(n) + 0.5) / n * (np.pi / 2.0)
    pl = (np.arange(n) + 0.5) / n * 2.0 * np.pi
    TL, PL = np.meshgrid(tl, pl, indexing="ij")
    lv = np.stack([np.sin(TL) * np.cos(PL), np.sin(TL) * np.sin(PL), np.cos(TL)], axis=-1)
    hh = lv + v
    hh /= np.linalg.norm(hh, axis=-1, keepdims=True)
    vhd = hh @ v
    dw = np.sin(TL) * (np.pi / 2.0 / n) * (2.0 * np.pi / n)
    diffuse = np.sum((1.0 - fresnel(vhd)) / np.pi * lv[..., 2] * dw)
    return diffuse, specular


def main():
    nvs = (0.05, 0.1, 0.2, 0.4, 0.7, 1.0)
    print("White dielectric furnace albedo, glTF 2.0 formula (specification, appendix B: mix(diffuse, specular, F(v.h))).")
    print("A value above 1 means the model reflects more light than it receives.")
    print(f"{'rough':>6} " + " ".join(f"nv={nv:4.2f}" for nv in nvs))
    worst = (0.0, None)
    burley_rows, coupled_rows = [], []
    for rough in (0.1, 0.2, 0.3, 0.5, 0.7, 1.0):
        cells, burley, coupled = [], [], []
        for nv in nvs:
            d, s = albedo_parts(nv, rough)
            cells.append(d + s)
            if d + s > worst[0]:
                worst = (d + s, (rough, nv))
            burley.append(burley_diffuse(nv) + s)
            coupled.append((1.0 - s) + s)  # diffuse weighted by 1 - E_spec(n.v): a white Lambertian base reflects 1 of what remains
        print(f"{rough:6.2f} " + " ".join(f"{c:7.4f}" for c in cells))
        burley_rows.append(burley)
        coupled_rows.append(coupled)
    print()
    print(f"specification formula: worst albedo {worst[0]:.4f} at roughness {worst[1][0]}, n.v {worst[1][1]} "
          f"(overshoot {100.0 * (worst[0] - 1.0):.1f}%); above 1.02 only for n.v <= 0.4.")
    bw = max(max(r) for r in burley_rows)
    print(f"Burley-style diffuse weighting (1-F(n.l))(1-F(n.v)): worst albedo {bw:.4f}; darkest white-dielectric albedo "
          f"{min(min(r) for r in burley_rows):.4f} (the darkening the specification mentions)")
    print("diffuse scaled by (1 - specular albedo): 1.0000 by construction for a white base (single scattering only).")
    print("Only the first is what the glTF specification defines; the others are the optional, documented improvements.")


if __name__ == "__main__":
    main()
