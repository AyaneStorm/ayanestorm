#!/usr/bin/env python3
"""
@file as_build_milky_way.py
@author chanayane@firestorm
@brief Builds the AyaneStorm real-sky Milky Way / deep-sky glow texture from
the Celestial Data sets (Frohn & Hernangomez 2023, BSD 3-Clause,
https://doi.org/10.5281/zenodo.7561601), itself from d3-celestial.

Usage:
    python as_build_milky_way.py <celestial data dir> <out.png> [--preview p.png]
                                 [--no-mottle] [--seed N]

Inputs (from <celestial data dir>): mw.min.geojson (5 nested Milky Way
brightness outlines ol1..ol5), dsos.bright.min.geojson (hand-picked bright
deep-sky objects), lg.min.geojson (Local Group: LMC and SMC).

Output: 2048x2048 RGB PNG (no alpha, viewable as is), two stacked 2048x1024
equirectangular maps in J2000 equatorial coordinates: top half = Milky Way,
bottom half = deep-sky objects, so the viewer scales them separately. In each
half x = (RA + 180) / 360 with RA wrapped to [-180, 180] (the GeoJSON
longitude) and north at the top. RGB = linear glow color. The viewer's PNG
decoder flips rows, so in GL the Milky Way half is v in [0.5, 1] and the
deep-sky half v in [0, 0.5], each with v = (dec + 90) / 180 inside its half.
Requires numpy and Pillow.
"""

import argparse
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw

W, H = 2048, 1024

# Milky Way levels: cumulative weight added by each nested outline and its
# blur radius (degrees); outer levels are wider and softer.
MW_LEVEL_WEIGHT = [0.18, 0.18, 0.2, 0.2, 0.24]
MW_LEVEL_BLUR_DEG = [3.0, 2.0, 1.3, 0.8, 0.5]
# Linear tint from the faint outer glow to the bright core.
MW_OUTER_TINT = np.array([0.78, 0.84, 1.0])
MW_CORE_TINT = np.array([1.0, 0.86, 0.68])
MOTTLE_STRENGTH = 0.35

# Deep-sky tints (linear) and weights by object type.
DSO_STYLE = {
    "s": ((1.0, 0.9, 0.78), 1.0),     # spiral galaxy
    "sd": ((1.0, 0.9, 0.78), 1.0),
    "i": ((0.95, 0.92, 0.95), 1.0),   # irregular galaxy (Magellanic Clouds)
    "e": ((1.0, 0.88, 0.75), 1.0),
    "g": ((1.0, 0.9, 0.78), 1.0),
    "sfr": ((1.0, 0.55, 0.62), 1.0),  # star-forming region (emission)
    "en": ((1.0, 0.5, 0.58), 1.0),    # emission nebula
    "bn": ((1.0, 0.6, 0.65), 1.0),
    "rn": ((0.6, 0.72, 1.0), 1.0),    # reflection nebula
    "gc": ((1.0, 0.93, 0.8), 0.8),    # globular cluster
    "oc": ((0.82, 0.88, 1.0), 0.25),  # open cluster: stars already drawn
}
# Data fixes: NGC 6121 (M4) carries M42's coordinates in dsos.bright.
POSITION_OVERRIDE = {"NGC 6121": (245.897 - 360.0, -26.526)}
# Objects taken from lg.min.geojson instead (better dimensions).
SKIP_BRIGHT = {"PGC 17223", "NGC 292", "GC"}
LG_OBJECTS = {"LMC", "SMC"}
# Peak brightness given to the LMC; others scale by surface brightness.
LMC_PEAK = 0.5


def to_px(lon, lat):
    return (lon + 180.0) / 360.0 * W, (90.0 - lat) / 180.0 * H


def row_latitudes():
    return 90.0 - (np.arange(H) + 0.5) / H * 180.0


def blur(img, sigma_deg):
    """Gaussian blur in degrees on the sphere: horizontal sigma widened by
    1/cos(dec) per row (wrapping in RA), vertical zero-padded."""
    px_per_deg_x = W / 360.0
    px_per_deg_y = H / 180.0
    cos_lat = np.maximum(np.cos(np.radians(row_latitudes())), 0.05)
    sx = sigma_deg * px_per_deg_x / cos_lat                     # per row
    fx = np.fft.rfftfreq(W)
    spec = np.fft.rfft(img, axis=1)
    spec *= np.exp(-2.0 * (math.pi ** 2) * (sx[:, None] ** 2) * (fx[None, :] ** 2))
    img = np.fft.irfft(spec, n=W, axis=1)
    sy = sigma_deg * px_per_deg_y
    pad = np.zeros((2 * H, W))
    pad[:H] = img
    fy = np.fft.rfftfreq(2 * H)
    spec = np.fft.rfft(pad, axis=0)
    spec *= np.exp(-2.0 * (math.pi ** 2) * (sy ** 2) * (fy[:, None] ** 2))
    return np.fft.irfft(spec, n=2 * H, axis=0)[:H]


def periodic_noise(rng, cells_x):
    """Value noise, periodic in RA, smooth bilinear-smoothstep interpolation."""
    cells_y = max(cells_x // 2, 2)
    grid = rng.random((cells_y + 1, cells_x))
    gx = (np.arange(W) + 0.5) / W * cells_x
    gy = (np.arange(H) + 0.5) / H * cells_y
    x0 = np.floor(gx).astype(int)
    y0 = np.floor(gy).astype(int)
    tx = gx - x0
    ty = gy - y0
    tx = tx * tx * (3 - 2 * tx)
    ty = ty * ty * (3 - 2 * ty)
    x1 = (x0 + 1) % cells_x
    x0 %= cells_x
    y1 = np.minimum(y0 + 1, cells_y)
    a = grid[y0][:, x0] * (1 - tx) + grid[y0][:, x1] * tx
    b = grid[y1][:, x0] * (1 - tx) + grid[y1][:, x1] * tx
    return a * (1 - ty[:, None]) + b * ty[:, None]


def milky_way(data_dir, mottle, seed):
    with open(os.path.join(data_dir, "mw.min.geojson"), encoding="utf-8") as f:
        features = {feat["properties"]["id"]: feat for feat in json.load(f)["features"]}
    glow = np.zeros((H, W))
    for level, (name, weight, sigma) in enumerate(zip(["ol1", "ol2", "ol3", "ol4", "ol5"],
                                                      MW_LEVEL_WEIGHT, MW_LEVEL_BLUR_DEG)):
        geom = features[name]["geometry"]
        polys = geom["coordinates"] if geom["type"] == "MultiPolygon" else [geom["coordinates"]]
        mask = Image.new("L", (W, H), 0)
        draw = ImageDraw.Draw(mask)
        for poly in polys:
            draw.polygon([to_px(lon, lat) for lon, lat in poly[0]], fill=255)
            for hole in poly[1:]:
                draw.polygon([to_px(lon, lat) for lon, lat in hole], fill=0)
        glow += weight * blur(np.asarray(mask, dtype=np.float64) / 255.0, sigma)
    glow = np.clip(glow, 0.0, 1.0)

    if mottle:
        rng = np.random.default_rng(seed)
        noise = sum(periodic_noise(rng, c) * w for c, w in ((24, 0.5), (64, 0.3), (160, 0.2)))
        # Star-cloud structure, stronger toward the brighter inner levels.
        amount = MOTTLE_STRENGTH * np.sqrt(glow)
        glow *= 1.0 - amount + amount * 2.0 * noise

    core = np.clip(glow, 0.0, 1.0) ** 0.7
    rgb = glow[..., None] * (MW_OUTER_TINT * (1 - core[..., None]) + MW_CORE_TINT * core[..., None])
    return rgb


def parse_dim(dim):
    parts = str(dim).lower().split("x")
    a = float(parts[0])
    b = float(parts[1]) if len(parts) > 1 else a
    return a, b


def deep_sky(data_dir):
    objects = []
    with open(os.path.join(data_dir, "dsos.bright.min.geojson"), encoding="utf-8") as f:
        for feat in json.load(f)["features"]:
            p = feat["properties"]
            if p["id"] in SKIP_BRIGHT or p["type"] not in DSO_STYLE or p["mag"] is None or p["mag"] >= 99:
                continue
            lon, lat = POSITION_OVERRIDE.get(p["id"], feat["geometry"]["coordinates"])
            objects.append((p["id"], p["type"], p["mag"], parse_dim(p["dim"]), lon, lat))
    with open(os.path.join(data_dir, "lg.min.geojson"), encoding="utf-8") as f:
        for feat in json.load(f)["features"]:
            p = feat["properties"]
            if p["id"] in LG_OBJECTS:
                lon, lat = feat["geometry"]["coordinates"]
                objects.append((p["id"], "i", p["mag"], parse_dim(p["dim"]), lon, lat))

    # Surface brightness ~ flux / area, normalised to the LMC.
    def surface(mag, dim):
        return 10.0 ** (-0.4 * mag) / (dim[0] * dim[1])
    lmc = next(o for o in objects if o[0] == "LMC")
    scale = LMC_PEAK / surface(lmc[2], lmc[3])

    rgb = np.zeros((H, W, 3))
    lum = np.zeros((H, W))
    lats = row_latitudes()
    for name, kind, mag, dim, lon, lat in objects:
        tint, weight = DSO_STYLE[kind]
        peak = min(surface(mag, dim) * scale, 1.0) * weight
        # Dimensions are total extents in arcminutes: +-2 sigma.
        sig_a = dim[0] / 60.0 / 4.0
        sig_b = dim[1] / 60.0 / 4.0
        cx, cy = to_px(lon, lat)
        reach_y = int(4 * sig_b * H / 180.0) + 2
        rows = np.arange(max(int(cy) - reach_y, 0), min(int(cy) + reach_y + 1, H))
        for r in rows:
            dlat = lats[r] - lat
            cosl = max(math.cos(math.radians(lats[r])), 0.05)
            reach_x = int(4 * sig_a * W / 360.0 / cosl) + 2
            cols = np.arange(int(cx) - reach_x, int(cx) + reach_x + 1)
            dlon = ((cols + 0.5) / W * 360.0 - 180.0 - lon) * cosl
            val = peak * np.exp(-0.5 * ((dlon / sig_a) ** 2 + (dlat / sig_b) ** 2))
            cols %= W
            rgb[r, cols] += val[:, None] * np.array(tint)
            lum[r, cols] += val
    return rgb, lum


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("out")
    ap.add_argument("--preview")
    ap.add_argument("--no-mottle", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    mw = np.clip(milky_way(args.data_dir, not args.no_mottle, args.seed), 0.0, 1.0)
    dso_rgb, _ = deep_sky(args.data_dir)
    dso_rgb = np.clip(dso_rgb, 0.0, 1.0)

    # Top half Milky Way, bottom half deep-sky.
    out = np.vstack([mw, dso_rgb])
    Image.fromarray((out * 255.0 + 0.5).astype(np.uint8), "RGB").save(args.out, optimize=True)
    print("wrote %s (%d bytes)" % (args.out, os.path.getsize(args.out)))
    if args.preview:
        # Both layers combined, gamma-encoded, for eyeballing.
        rgb = np.clip(mw + dso_rgb, 0.0, 1.0)
        Image.fromarray((rgb ** (1 / 2.2) * 255).astype(np.uint8), "RGB").save(args.preview)


if __name__ == "__main__":
    main()
