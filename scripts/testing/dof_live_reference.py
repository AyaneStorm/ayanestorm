#!/usr/bin/env python3
"""Live DoF (mode 3) reference model. Author: chanayane@firestorm. numpy.

Gate for doc/ayanestorm-depth-of-field-live-plan.md, phase 0. Checks the
layer decomposition and the area-tap gathers against brute-force scatter
before any shader exists.

Model (all in gather-resolution pixels):
- A layer is stored as premultiplied sums, box-filtered into a mip chain
  (glGenerateMipmap): S = sum c w, W = sum w, E = sum w / r^2, M = sum w r.
- Taps: a centre tap plus rings k = 1..n of 6k taps at radius k s, with
  s = R / (n + 1/2). Every ring tap stands for an area of pi s^2 / 3 and
  the centre for pi s^2 / 4, so the taps tile the disc of radius R exactly.
- A tap reads its layer with trilinear filtering (GL texel centres) at
  lod = log2(LOD_SCALE s): an area sample whose footprint matches the tap
  spacing.
- Scatter-as-gather, energy weighted: a tap contributes
  area / pi * E * reach, where reach is the share of the tap's annulus
  inside the tap's radius r_t = sqrt(W / E) (reach_fraction()). Coverage is
  the clamped sum; colour is normalized by the same weights.
- Near layers use one kernel radius (the tile's dilated maximum). The far
  layer uses the pixel's own mean radius M / W: nearer (less blurred)
  surfaces in front occlude the spread of farther ones.

Ground truth: every source pixel splats a disc of its own radius (1 px
antialiased edge, normalized to its area), so energy is exact.

Usage:
  python dof_live_reference.py          run the tests
  python dof_live_reference.py survey   print error tables
"""

import math
import sys
import unittest

import numpy as np

# Footprint of an area tap relative to the tap spacing (trilinear tent).
LOD_SCALE = 1.0
# Ring counts of the quality presets (37, 91, 169 taps).
QUALITY_RINGS = (3, 5, 7)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / np.maximum(e1 - e0, 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------- mip chain

def build_mips(level0):
    """Box mip chain of a (H, W, C) array, as glGenerateMipmap (2x2 mean)."""
    mips = [level0]
    while min(mips[-1].shape[0], mips[-1].shape[1]) > 1:
        a = mips[-1]
        h, w = a.shape[0] // 2, a.shape[1] // 2
        a = a[:2 * h, :2 * w]
        mips.append(0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]))
    return mips


def sample_level(level, x, y):
    """Bilinear sample at level-0 pixel coordinates x, y (texel centres at
    i + 0.5), clamp to edge. x, y arrays of the same shape."""
    h, w = level.shape[0], level.shape[1]
    fx = x - 0.5
    fy = y - 0.5
    x0 = np.floor(fx)
    y0 = np.floor(fy)
    tx = (fx - x0)[..., None]
    ty = (fy - y0)[..., None]
    x0 = x0.astype(int)
    y0 = y0.astype(int)
    xa = np.clip(x0, 0, w - 1)
    xb = np.clip(x0 + 1, 0, w - 1)
    ya = np.clip(y0, 0, h - 1)
    yb = np.clip(y0 + 1, 0, h - 1)
    return ((level[ya, xa] * (1 - tx) + level[ya, xb] * tx) * (1 - ty) +
            (level[yb, xa] * (1 - tx) + level[yb, xb] * tx) * ty)


def sample_trilinear(mips, x, y, lod):
    """textureLod with LINEAR_MIPMAP_LINEAR; x, y in level-0 pixels."""
    lod = np.clip(lod, 0.0, len(mips) - 1)
    l0 = np.floor(lod).astype(int)
    t = (lod - l0)[..., None]
    out = np.zeros(x.shape + (mips[0].shape[2],))
    for level in range(len(mips)):
        sel0 = l0 == level
        sel1 = (l0 + 1 == level) & (t[..., 0] > 0)
        if not sel0.any() and not sel1.any():
            continue
        scale = 2.0 ** level
        value = sample_level(mips[level], x / scale, y / scale)
        out += np.where(sel0[..., None], value * (1 - t), 0.0)
        out += np.where(sel1[..., None], value * t, 0.0)
    return out


# ---------------------------------------------------------------- taps

def ring_taps(rings):
    """Unit-disc taps: (dx, dy, distance, area) with radius 1 = kernel R."""
    s = 1.0 / (rings + 0.5)
    taps = [(0.0, 0.0, 0.0, math.pi * s * s / 4.0)]
    for k in range(1, rings + 1):
        count = 6 * k
        offset = 0.5 if (k & 1) else 0.0
        for j in range(count):
            a = 2.0 * math.pi * (j + offset) / count
            taps.append((k * s * math.cos(a), k * s * math.sin(a), k * s,
                         math.pi * s * s / 3.0))
    return taps, s


# ---------------------------------------------------------------- layers

def make_layer(color, alpha, radius):
    """Per-pixel layer sums for one bin: channels S (rgb), W, E, M."""
    r = np.maximum(radius, 0.5)
    w = alpha
    return np.stack([color[..., 0] * w, color[..., 1] * w, color[..., 2] * w,
                     w, w / (r * r), w * r], axis=-1)


def reach_fraction(r, d, s, ring):
    """Share of a tap's area that a source of radius r reaches. A ring tap
    stands for the annulus [d - s/2, d + s/2], the centre tap for the disc
    of radius s/2; the share is the part of that area inside radius r, so
    the taps integrate pi r^2 exactly for any r (a smoothstep over one
    spacing lost up to 9% coverage for r between rings)."""
    if ring:
        return np.clip((r * r - (d - 0.5 * s) ** 2) / (2.0 * d * s), 0.0, 1.0)
    return np.clip(r * r / (0.25 * s * s), 0.0, 1.0)


def gather(mips, x, y, kernel_radius, rings, near):
    """Area-tap scatter-as-gather at pixels (x, y) (pixel centres).

    Returns (premultiplied rgb, coverage). kernel_radius: array per pixel.
    near: True for one shared kernel (coverage is the energy sum), False for
    the far layer (colour normalized, coverage clamped energy sum)."""
    taps, s_unit = ring_taps(rings)
    R = np.maximum(kernel_radius, 0.5)
    s = s_unit * R
    lod = np.log2(np.maximum(LOD_SCALE * s, 1.0))
    color = np.zeros(x.shape + (3,))
    weight = np.zeros(x.shape)
    for dx, dy, d_unit, area_unit in taps:
        d = d_unit * R
        area = area_unit * R * R
        # Foreground sources image as the inverted aperture: the source
        # reaching this pixel lies at +offset (near) or -offset (far). For a
        # circular aperture the sign does not change the result.
        sx = x + (dx if near else -dx) * R
        sy = y + (dy if near else -dy) * R
        v = sample_trilinear(mips, sx, sy, lod)
        W = v[..., 3]
        E = v[..., 4]
        r_tap = np.sqrt(W / np.maximum(E, 1e-12))
        reach = reach_fraction(r_tap, d, s, d_unit > 0)
        wt = area / math.pi * E * reach
        rgb = v[..., 0:3] / np.maximum(W, 1e-12)[..., None]
        color += rgb * wt[..., None]
        weight += wt
    coverage = np.clip(weight, 0.0, 1.0)
    rgb = color / np.maximum(weight, 1e-12)[..., None]
    return rgb * coverage[..., None], coverage, rgb


# ---------------------------------------------------------------- truth

def scatter_truth(color, alpha, radius):
    """Brute-force splat: each source spreads alpha over its own disc
    (1 px antialiased edge), normalized to the disc's area."""
    h, w = alpha.shape
    acc = np.zeros((h, w, 3))
    cov = np.zeros((h, w))
    yy, xx = np.mgrid[0:h, 0:w]
    ys, xs = np.nonzero(alpha > 0)
    for py, px in zip(ys, xs):
        r = max(radius[py, px], 0.5)
        ext = int(math.ceil(r + 1))
        y0, y1 = max(py - ext, 0), min(py + ext + 1, h)
        x0, x1 = max(px - ext, 0), min(px + ext + 1, w)
        dist = np.hypot(xx[y0:y1, x0:x1] - px, yy[y0:y1, x0:x1] - py)
        k = np.clip(r + 0.5 - dist, 0.0, 1.0)
        # Normalize by the full disc area (also outside the frame).
        k = k * alpha[py, px] / (math.pi * r * r)
        acc[y0:y1, x0:x1] += color[py, px] * k[..., None]
        cov[y0:y1, x0:x1] += k
    coverage = np.clip(cov, 0.0, 1.0)
    rgb = acc / np.maximum(cov, 1e-12)[..., None]
    return rgb * coverage[..., None], coverage, rgb, cov


# ---------------------------------------------------------------- scenes

SIZE = 96


def scene(name):
    """Returns (color, alpha, radius) of one near layer, plus a label."""
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    color = np.zeros((h, w, 3))
    alpha = np.zeros((h, w))
    radius = np.zeros((h, w))
    if name == "uniform":
        alpha[:] = 1.0
        radius[:] = 12.0
        color[:] = (0.8, 0.4, 0.2)
    elif name == "edge":
        sel = xx < w / 2
        alpha[sel] = 1.0
        radius[sel] = 12.0
        color[sel] = (0.9, 0.9, 0.2)
    elif name == "strands":
        # Thin 1 px strands, every 4 px, blurred 14 px.
        sel = (xx.astype(int) % 4) == 0
        alpha[sel] = 1.0
        radius[sel] = 14.0
        color[sel] = (0.1, 0.05, 0.0)
    elif name == "lockface":
        # A face blurred 2.5 px with a lock of 1 px strands blurred 14 px:
        # both in the same near bin (the hard mixed-radius case).
        alpha[:] = 1.0
        radius[:] = 2.5
        color[:] = (1.0, 0.8, 0.7)
        sel = ((xx.astype(int) % 3) == 0) & (np.abs(xx - w / 2) < 16)
        radius[sel] = 14.0
        color[sel] = (0.05, 0.03, 0.0)
    elif name == "lockface_split":
        # The same after the geometric N1/N2 split: what shares a bin spans
        # at most a radius ratio of about sqrt(near_max / 2) (here 2.5 and 8).
        alpha[:] = 1.0
        radius[:] = 2.5
        color[:] = (1.0, 0.8, 0.7)
        sel = ((xx.astype(int) % 3) == 0) & (np.abs(xx - w / 2) < 16)
        radius[sel] = 8.0
        color[sel] = (0.05, 0.03, 0.0)
    elif name == "mixed":
        # Two foreground surfaces, 4 and 20 px, side by side.
        alpha[:] = 1.0
        radius[:] = 4.0
        radius[xx >= w / 2] = 20.0
        color[:] = (0.2, 0.6, 0.9)
        color[xx >= w / 2] = (0.9, 0.3, 0.1)
    elif name == "light":
        # One bright isolated light, 10 px blur, on nothing.
        alpha[h // 2, w // 2] = 1.0
        radius[h // 2, w // 2] = 10.0
        color[h // 2, w // 2] = (50.0, 40.0, 30.0)
    else:
        raise ValueError(name)
    return color, alpha, radius


# Tile side in gather pixels (16 full-resolution px at half resolution).
TILE = 8


def tile_kernel(radius, alpha):
    """Per-pixel near kernel radius: the tile maximum, dilated by the
    largest radius (in tiles), as the tile passes compute it."""
    h, w = radius.shape
    th, tw = (h + TILE - 1) // TILE, (w + TILE - 1) // TILE
    tmax = np.zeros((th, tw))
    r = np.where(alpha > 0, radius, 0.0)
    for ty in range(th):
        for tx in range(tw):
            tmax[ty, tx] = r[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE].max()
    reach = int(math.ceil(r.max() / TILE))
    dil = np.zeros_like(tmax)
    for ty in range(th):
        for tx in range(tw):
            dil[ty, tx] = tmax[max(ty - reach, 0):ty + reach + 1,
                               max(tx - reach, 0):tx + reach + 1].max()
    return np.kron(dil, np.ones((TILE, TILE)))[:h, :w]


def evaluate(name, rings, background=(0.5, 0.5, 0.5)):
    color, alpha, radius = scene(name)
    t_pre, t_cov, _, t_energy = scatter_truth(color, alpha, radius)
    mips = build_mips(make_layer(color, alpha, radius))
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    R = tile_kernel(radius, alpha)
    g_pre, g_cov, _ = gather(mips, xx + 0.5, yy + 0.5, R, rings, near=True)
    bg = np.array(background)
    t_img = t_pre + (1 - t_cov)[..., None] * bg
    g_img = g_pre + (1 - g_cov)[..., None] * bg
    # Interior only: the truth normalizes off-frame area, the gather clamps.
    m = int(radius.max()) + 2
    sl = (slice(m, SIZE - m), slice(m, SIZE - m))
    cov_rms = float(np.sqrt(np.mean((g_cov[sl] - t_cov[sl]) ** 2)))
    img_rms = float(np.sqrt(np.mean((g_img[sl] - t_img[sl]) ** 2)))
    return cov_rms, img_rms, g_cov, t_cov, t_energy


# Minimum weight sum for a mip level to define a pixel's own far radius.
FAR_RADIUS_MIN_WEIGHT = 0.25


def far_kernel(mips, x, y):
    """Far kernel radius: the pixel's mean radius M / W at the finest level
    holding enough far content, so holes (in-focus or foreground pixels)
    take the radius of the background around them."""
    radius = np.zeros(x.shape)
    found = np.zeros(x.shape, dtype=bool)
    for level, m in enumerate(mips):
        scale = 2.0 ** level
        v = sample_level(m, x / scale, y / scale)
        ok = (~found) & (v[..., 3] >= FAR_RADIUS_MIN_WEIGHT)
        radius[ok] = v[ok][..., 5] / v[ok][..., 3]
        found |= ok
    return radius


def far_scene(name):
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    color = np.zeros((h, w, 3))
    alpha = np.ones((h, w))
    color[:] = 0.2
    color[(((xx // 6) + (yy // 6)) % 2) == 1] = (0.9, 0.7, 0.3)
    if name == "far_hole":
        radius = np.full((h, w), 10.0)
        # An in-focus object (in bin F) leaves a hole in the far layer.
        hole = (np.abs(xx - w / 2) < 10) & (np.abs(yy - h / 2) < 10)
        alpha[hole] = 0.0
    elif name == "far_ramp":
        radius = 2.0 + 14.0 * xx / (w - 1)
    else:
        raise ValueError(name)
    return color, alpha, radius


def evaluate_far(name, rings):
    color, alpha, radius = far_scene(name)
    _, _, t_rgb, t_energy = scatter_truth(color, alpha, radius)
    mips = build_mips(make_layer(color, alpha, radius))
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    x, y = xx + 0.5, yy + 0.5
    R = far_kernel(mips, x, y)
    _, _, g_rgb = gather(mips, x, y, R, rings, near=False)
    m = 18
    sl = (slice(m, SIZE - m), slice(m, SIZE - m))
    valid = t_energy[sl] > 0.5
    err = np.abs(g_rgb[sl] - t_rgb[sl]).max(axis=-1)[valid]
    return float(np.sqrt(np.mean(err ** 2))), float(err.max())


class LiveDoFTests(unittest.TestCase):
    def test_taps_tile_the_disc(self):
        for rings in QUALITY_RINGS:
            taps, _ = ring_taps(rings)
            self.assertAlmostEqual(sum(t[3] for t in taps), math.pi, places=12)
            self.assertEqual(len(taps), 1 + 3 * rings * (rings + 1))

    def test_mips_preserve_sums(self):
        rng = np.random.default_rng(1)
        a = rng.random((64, 64, 6))
        mips = build_mips(a)
        for m in mips:
            self.assertAlmostEqual(float(m.mean()), float(a.mean()), places=12)

    def test_uniform_layer_is_opaque(self):
        for rings in QUALITY_RINGS:
            cov, img, g_cov, _, _ = evaluate("uniform", rings)
            self.assertLess(cov, 0.02, rings)
            self.assertLess(img, 0.01, rings)

    def test_edge(self):
        for rings in QUALITY_RINGS:
            cov, img, _, _, _ = evaluate("edge", rings)
            self.assertLess(cov, 0.005, rings)
            self.assertLess(img, 0.002, rings)

    def test_strands_veil(self):
        for rings in QUALITY_RINGS:
            cov, img, g_cov, t_cov, _ = evaluate("strands", rings)
            self.assertLess(cov, 0.01, rings)
            self.assertLess(img, 0.005, rings)

    def test_lock_over_face(self):
        # Mixed radii in one bin: a smooth bias, not noise. With the
        # geometric N1/N2 split the bin's radius ratio stays small.
        for rings in QUALITY_RINGS:
            _, img, _, _, _ = evaluate("lockface", rings)
            self.assertLess(img, 0.05, rings)
            _, img, _, _, _ = evaluate("lockface_split", rings)
            self.assertLess(img, 0.04, rings)

    def test_mixed_radii_edge(self):
        for rings in QUALITY_RINGS:
            cov, img, _, _, _ = evaluate("mixed", rings)
            self.assertLess(cov, 0.08, rings)
            self.assertLess(img, 0.035, rings)

    def test_light_energy(self):
        # The light's energy (coverage integral) is preserved.
        for rings in QUALITY_RINGS:
            _, _, g_cov, t_cov, t_energy = evaluate("light", rings)
            self.assertAlmostEqual(float(g_cov.sum()), float(t_energy.sum()), delta=0.02)

    def test_far_layer(self):
        for name in ("far_hole", "far_ramp"):
            for rings in QUALITY_RINGS:
                rms, _ = evaluate_far(name, rings)
                self.assertLess(rms, 0.04, (name, rings))

    def test_bin_split_restores_coverage(self):
        # An opaque surface split 50/50 between two adjacent bins: the back
        # bin alone is (W, S) / V with V = 1 - W_front, so it is opaque again
        # and front-over-back returns the surface's colour, unlike plain
        # over-compositing of the split weights (0.5 + 0.5 * 0.5 = 0.75).
        c = np.array([0.3, 0.6, 0.9])
        w_front, w_back = 0.5, 0.5
        v_back = 1.0 - w_front
        back_alone_alpha = w_back / v_back
        back_alone_rgb = c * w_back / v_back
        out = c * w_front + (1 - w_front) * back_alone_rgb
        self.assertAlmostEqual(back_alone_alpha, 1.0)
        np.testing.assert_allclose(out, c)

    def test_decomposition_identity(self):
        # Fragments with exact source-over weights binned by depth: the bins
        # plus T * opaque reproduce sorted compositing.
        rng = np.random.default_rng(7)
        for _ in range(200):
            n = rng.integers(1, 6)
            depth = np.sort(rng.random(n))
            a = rng.random(n)
            c = rng.random((n, 3))
            opaque = rng.random(3)
            ref = opaque.copy()
            for i in reversed(range(n)):
                ref = c[i] * a[i] + (1 - a[i]) * ref
            front = np.concatenate([[1.0], np.cumprod(1 - a)[:-1]])
            w = a * front
            T = float(np.prod(1 - a))
            bins = np.minimum((depth * 4).astype(int), 3)
            S = np.zeros((4, 3))
            for i in range(n):
                S[bins[i]] += c[i] * w[i]
            np.testing.assert_allclose(S.sum(axis=0) + T * opaque, ref, atol=1e-12)


def survey():
    print(f"LOD_SCALE {LOD_SCALE}; SIZE {SIZE}")
    print(f"{'scene':10s} {'rings':>5s} {'cov rms':>9s} {'img rms':>9s}")
    for name in ("uniform", "edge", "strands", "lockface", "lockface_split",
                 "mixed", "light"):
        for rings in QUALITY_RINGS:
            cov, img, g_cov, t_cov, t_energy = evaluate(name, rings)
            extra = ""
            if name == "light":
                extra = f"  energy {g_cov.sum():.4f} / {t_energy.sum():.4f}"
            print(f"{name:10s} {rings:5d} {cov:9.4f} {img:9.4f}{extra}")
    print(f"{'far scene':10s} {'rings':>5s} {'rgb rms':>9s} {'rgb max':>9s}")
    for name in ("far_hole", "far_ramp"):
        for rings in QUALITY_RINGS:
            rms, mx = evaluate_far(name, rings)
            print(f"{name:10s} {rings:5d} {rms:9.4f} {mx:9.4f}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "survey":
        if len(sys.argv) > 2:
            LOD_SCALE = float(sys.argv[2])
        survey()
    else:
        unittest.main()
