#!/usr/bin/env python3
"""
@file as_build_milky_way.py
@author chanayane@firestorm
@brief Builds the AyaneStorm real-sky Milky Way and deep-sky glow textures.

Sources:
- Celestial Data (Frohn & Hernangomez 2023, BSD 3-Clause,
  https://doi.org/10.5281/zenodo.7561601, from d3-celestial): Milky Way
  outlines (mw), bright deep-sky objects (dsos.bright), Local Group (lg).
- Optional photographic band (--photo): NASA SVS Deep Star Maps 2020
  milkyway_2020_*.exr (https://svs.gsfc.nasa.gov/4851; credit: NASA/Goddard
  Space Flight Center Scientific Visualization Studio. Gaia DR2:
  ESA/Gaia/DPAC), linear half-float plate carree, ICRF/J2000, RA 0h at the
  center, RA increasing to the left; Gaia DR2 stars only (no stars brighter
  than magnitude 11.5).

Usage:
    python as_build_milky_way.py <celestial data dir> <out dir>
        [--photo milkyway_2020_8k.exr] [--preview p.png] [--no-mottle] [--seed N]

Outputs in <out dir>, equirectangular J2000, x = (RA + 180) / 360 with RA
wrapped to [-180, 180] (the GeoJSON longitude, RA increasing to the right),
north at the top (the viewer's PNG decoder flips rows, so GL v = (dec + 90) /
180), RGB gamma-encoded (value^(1/2.2), decoded in the shader) so 8 bits do
not band in the faint glow:
- as_milky_way.jpg: the band, 4096x2048 from --photo, else 2048x1024 from
  the outlines. JPEG q90 without chroma subsampling: the photo's faint-star
  texture does not compress as PNG (16.8 MB vs 3.2 MB, mean error 3/255).
- as_deep_sky.png: 2048x1024 deep-sky glow. With --photo only nebulae (gas
  glow is not in star data; the photo already shows the Magellanic Clouds,
  Andromeda, Triangulum and the clusters as stars).
Requires numpy and Pillow (+ OpenEXR for --photo).
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


def deep_sky(data_dir, kinds=None):
    """Deep-sky glow; kinds: object types to draw (None = all)."""
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
        if kinds is not None and kind not in kinds:
            continue
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


# Photo mode: nebula types kept in the deep-sky layer.
NEBULA_KINDS = {"sfr", "en", "bn", "rn"}
PHOTO_W, PHOTO_H = 4096, 2048
# Photo normalisation: this luminance percentile maps to 1.
PHOTO_WHITE_PERCENTILE = 99.99


def photo_band(path, outline_band):
    """Loads the NASA SVS milkyway_2020 EXR, checks its orientation against
    the outline band, flips it to our RA-to-the-right layout and resamples it
    to PHOTO_W x PHOTO_H (area average)."""
    import OpenEXR
    with OpenEXR.File(path) as f:
        rgb = f.channels()["RGB"].pixels.astype(np.float32)

    def resize(img, w, h):
        return np.dstack([np.asarray(Image.fromarray(np.ascontiguousarray(img[..., c])).resize((w, h), Image.BOX))
                          for c in range(3)])

    # Orientation check: circular cross-correlation in RA against the
    # outline band for the four flips; expect RA flipped, no shift.
    lum = resize(rgb, W, H).mean(axis=2)
    ref = outline_band.mean(axis=2)
    ref = (ref - ref.mean()) / ref.std()
    best = None
    for name, img in (("as is", lum), ("flip RA", lum[:, ::-1]),
                      ("flip Dec", lum[::-1]), ("flip both", lum[::-1, ::-1])):
        a = np.log1p(img / max(np.median(img), 1e-9))
        a = (a - a.mean()) / a.std()
        corr = np.fft.irfft(np.fft.rfft(a, axis=1) * np.conj(np.fft.rfft(ref, axis=1)), n=W, axis=1).sum(axis=0) / a.size
        shift = int(np.argmax(corr))
        shift_deg = (shift if shift <= W // 2 else shift - W) * 360.0 / W
        print("  orientation %-9s corr %.3f shift %+.1f deg" % (name, corr[shift], shift_deg))
        if best is None or corr[shift] > best[1]:
            best = (name, corr[shift], shift_deg)
    if best[0] != "flip RA" or abs(best[2]) > 3.0:
        raise SystemExit("unexpected orientation %s (shift %.1f deg): not the documented SVS layout" % (best[0], best[2]))

    band = resize(rgb[:, ::-1], PHOTO_W, PHOTO_H)
    white = np.percentile(band.mean(axis=2), PHOTO_WHITE_PERCENTILE)
    print("  photo white point %.4g (luminance p%g)" % (white, PHOTO_WHITE_PERCENTILE))
    return np.clip(band / white, 0.0, 1.0)


def save_gamma(path, rgb):
    img = Image.fromarray((np.clip(rgb, 0.0, 1.0) ** (1.0 / 2.2) * 255.0 + 0.5).astype(np.uint8), "RGB")
    if path.lower().endswith(".jpg"):
        img.save(path, "JPEG", quality=90, subsampling=0, optimize=True)
    else:
        img.save(path, optimize=True)
    print("wrote %s %dx%d (%d bytes)" % (path, rgb.shape[1], rgb.shape[0], os.path.getsize(path)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--photo", help="NASA SVS milkyway_2020_*.exr (celestial)")
    ap.add_argument("--preview")
    ap.add_argument("--no-mottle", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    outline = np.clip(milky_way(args.data_dir, not args.no_mottle, args.seed), 0.0, 1.0)
    if args.photo:
        band = photo_band(args.photo, outline)
        dso_rgb, _ = deep_sky(args.data_dir, NEBULA_KINDS)
    else:
        band = outline
        dso_rgb, _ = deep_sky(args.data_dir)
    dso_rgb = np.clip(dso_rgb, 0.0, 1.0)

    save_gamma(os.path.join(args.out_dir, "as_milky_way.jpg"), band)
    save_gamma(os.path.join(args.out_dir, "as_deep_sky.png"), dso_rgb)
    if args.preview:
        # Both layers combined at the deep-sky resolution, for eyeballing.
        small = np.dstack([np.asarray(Image.fromarray(np.ascontiguousarray(band[..., c]).astype(np.float32)).resize((W, H), Image.BOX))
                           for c in range(3)])
        save_gamma(args.preview, small + dso_rgb)


if __name__ == "__main__":
    main()
