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
- The background is split like the foreground, geometrically: B1 (near
  background, gathered like a near layer, a veil) over B2 (far background,
  own kernel, normalized). One background bin blurred whatever showed
  between nearly sharp strands by the strands' small radius
  (evaluate_far_strands()).
- Every read is visibility-completed (read_completed()), by the push-pull
  recurrence c(l) = S(l) + (1 - V(l)) c(l + 1), ending with S / V at the
  top level: what a bin shows (V = 1) is read exactly, in one fetch; what
  nearer bins hide of it is filled from coarser levels, continuously, in
  proportion to what is missing. Every gather and the composite's focus
  layer read layers this way. A threshold rule ("the first level where at
  least half the footprint shows the bin") was measured first and fails
  inside any hole as large as the visible part: no level ever qualifies.

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

def make_layer(color, alpha, radius, visibility=None):
    """Per-pixel layer sums for one bin: channels S (rgb), W, E, M, V.
    alpha is the bin's visible weight; V the visibility in front of the bin
    (1 for the front bin)."""
    r = np.maximum(radius, 0.5)
    w = alpha
    v = np.ones_like(alpha) if visibility is None else visibility
    return np.stack([color[..., 0] * w, color[..., 1] * w, color[..., 2] * w,
                     w, w / (r * r), w * r, v], axis=-1)


# Push-pull stops once the hidden share left is below this
# (asDoFLiveCommonF.glsl, LIVE_COMPLETE_EPSILON).
COMPLETE_EPSILON = 0.01


def read_completed(mips, x, y, lod):
    """The bin alone at lod, visibility-completed by push-pull:
    c(l) = S(l) + (1 - V(l)) c(l + 1), with S / V at the top level, read
    front to back with the hidden share t = prod (1 - V). Returns
    (values, found); found is false only where no level shows the bin."""
    top = len(mips) - 1
    out = np.zeros(x.shape + (6,))
    hidden = np.ones(x.shape)
    for step in range(top + 1):
        level = np.minimum(lod + step, top)
        v = sample_trilinear(mips, x, y, level)
        vis = v[..., 6]
        last = level >= top
        # Top level: normalize what is left by its visibility.
        share = np.where(last, hidden / np.maximum(vis, 1e-6) * (vis > 1e-6), hidden)
        active = hidden > COMPLETE_EPSILON
        out += np.where(active[..., None], v[..., 0:6] * share[..., None], 0.0)
        hidden = np.where(active & ~last, hidden * (1.0 - vis), 0.0)
        if not (hidden > COMPLETE_EPSILON).any():
            break
    found = out[..., 3] > 1e-6
    return out, found


def read_raw(mips, x, y, lod):
    """The previous read (before completion): the sums divided by V at the
    tap's own level, nothing where the bin is hidden there."""
    v = sample_trilinear(mips, x, y, lod)
    found = v[..., 6] >= 0.001
    out = np.zeros(x.shape + (6,))
    out[found] = v[found][..., 0:6] / v[found][..., 6:7]
    return out, found


def reach_fraction(r, d, s, ring):
    """Share of a tap's area that a source of radius r reaches. A ring tap
    stands for the annulus [d - s/2, d + s/2], the centre tap for the disc
    of radius s/2; the share is the part of that area inside radius r, so
    the taps integrate pi r^2 exactly for any r (a smoothstep over one
    spacing lost up to 9% coverage for r between rings)."""
    if ring:
        return np.clip((r * r - (d - 0.5 * s) ** 2) / (2.0 * d * s), 0.0, 1.0)
    return np.clip(r * r / (0.25 * s * s), 0.0, 1.0)


def gather(mips, x, y, kernel_radius, rings, near, complete=True):
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
        v, found = (read_completed if complete else read_raw)(mips, sx, sy, lod)
        W = np.where(found, v[..., 3], 0.0)
        E = np.where(found, v[..., 4], 0.0)
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


# Minimum weight sum for a mip level to define a pixel's own far radius
# (previous, uncompleted kernel only).
FAR_RADIUS_MIN_WEIGHT = 0.25


def far_kernel(mips, x, y, complete=True):
    """Far kernel radius: the pixel's mean radius M / W, completed, so holes
    (in-focus or foreground pixels) take the radius of the background around
    them. The previous kernel searched raw W instead."""
    if complete:
        v, found = read_completed(mips, x, y, np.zeros(x.shape))
        return np.where(found & (v[..., 3] > 1e-6), v[..., 5] / np.maximum(v[..., 3], 1e-6), 0.0)
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
    # Where the background is absent, a nearer bin (focus) hides it: V = 0.
    mips = build_mips(make_layer(color, alpha, radius, visibility=alpha))
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    x, y = xx + 0.5, yy + 0.5
    R = far_kernel(mips, x, y)
    _, _, g_rgb = gather(mips, x, y, R, rings, near=False)
    m = 18
    sl = (slice(m, SIZE - m), slice(m, SIZE - m))
    valid = t_energy[sl] > 0.5
    err = np.abs(g_rgb[sl] - t_rgb[sl]).max(axis=-1)[valid]
    return float(np.sqrt(np.mean(err ** 2))), float(err.max())


def evaluate_rock(rings, complete, near_radius=12.0, back_radius=1.5, pattern=False):
    """A solid foreground (left half, N2) blurred near_radius over a
    background blurred back_radius (B, hidden behind the foreground: V = 0
    there). Truth: the foreground's scatter over the whole background's
    scatter (the hidden part included). Returns the rms image error over the
    foreground's fringe, where its veil is partly transparent.

    The previous far gather (complete=False) found no background inside the
    hole and the composite fell back to the sharp source, the foreground
    itself: a hard edge where the veil fades (the user's foreground rock)."""
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    rock = xx < w / 2
    rock_color = np.array([0.30, 0.35, 0.30])
    # Smooth background (sky to sea): what the rock hides is predictable from
    # what shows around it. pattern=True adds a fine checker no method can
    # recover behind the rock (reported, not gated).
    t = (yy / (h - 1))[..., None]
    back_color = (1 - t) * np.array([0.55, 0.70, 0.95]) + t * np.array([0.85, 0.75, 0.55])
    if pattern:
        back_color = back_color.copy()
        back_color[(((xx // 4) + (yy // 4)) % 2) == 1] *= 0.6

    near_c = np.zeros((h, w, 3))
    near_c[rock] = rock_color
    near_a = rock.astype(float)
    near_r = np.full((h, w), near_radius)
    t_pre, t_cov, _, _ = scatter_truth(near_c, near_a, near_r)
    _, _, t_back, _ = scatter_truth(back_color, np.ones((h, w)), np.full((h, w), back_radius))
    truth = t_pre + (1 - t_cov)[..., None] * t_back

    near_mips = build_mips(make_layer(near_c, near_a, near_r))
    back_vis = (~rock).astype(float)
    back_mips = build_mips(make_layer(back_color, back_vis, np.full((h, w), back_radius),
                                      visibility=back_vis))
    x, y = xx + 0.5, yy + 0.5
    g_pre, g_cov, _ = gather(near_mips, x, y, tile_kernel(near_r, near_a), rings, near=True)
    kernel = np.maximum(far_kernel(back_mips, x, y, complete), 0.0)
    f_pre, f_cov, f_rgb = gather(back_mips, x, y, kernel, rings, near=False, complete=complete)
    if complete:
        back = f_rgb
    else:
        # Previous composite: no far result -> the source pixel (sharp).
        source = np.where(rock[..., None], rock_color, back_color)
        back = np.where(((f_cov > 0.0001) & (kernel >= 0.5))[..., None], f_rgb, source)
    image = g_pre + (1 - g_cov)[..., None] * back
    # The truth leaves off-frame sources out, the renderer continues the
    # frame's edge content: score away from all four borders.
    fringe = (t_cov > 0.02) & (t_cov < 0.98)
    fringe[:14] = False
    fringe[-14:] = False
    fringe[:, :14] = False
    fringe[:, -14:] = False
    return float(np.sqrt(np.mean((image[fringe] - truth[fringe]) ** 2)))


def evaluate_far_strands(rings, split):
    """Nearly sharp strands (behind the focus, radius 1) in front of a far
    background (radius 8): the user's rock between hair strands. Truth: the
    strands' scatter over the whole background's scatter. split=False: one
    background bin, own mean-radius kernel (the strands and the background
    mix in it); split=True: B1 (strands, tile kernel, a veil) over B2 (the
    background, completed behind the strands). Returns the rms image error
    away from the borders."""
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    back_color = np.zeros((h, w, 3))
    back_color[:] = (0.75, 0.72, 0.70)
    back_color[(((xx // 3) + (yy // 3)) % 2) == 1] = (0.25, 0.25, 0.22)
    strand = (xx.astype(int) % 4) == 0
    strand_alpha = np.where(strand, 0.8, 0.0)
    strand_color = np.zeros((h, w, 3))
    strand_color[:] = (0.30, 0.18, 0.08)
    strand_r = np.full((h, w), 1.0)
    back_r = np.full((h, w), 8.0)

    s_pre, s_cov, _, _ = scatter_truth(strand_color, strand_alpha, strand_r)
    _, _, t_back, _ = scatter_truth(back_color, np.ones((h, w)), back_r)
    truth = s_pre + (1 - s_cov)[..., None] * t_back

    x, y = xx + 0.5, yy + 0.5
    back_w = 1.0 - strand_alpha
    if split:
        b1 = build_mips(make_layer(strand_color, strand_alpha, strand_r))
        b2 = build_mips(make_layer(back_color, back_w, back_r, visibility=back_w))
        b1_pre, b1_cov, _ = gather(b1, x, y, tile_kernel(strand_r, strand_alpha), rings,
                                   near=False)
        _, _, b2_rgb = gather(b2, x, y, far_kernel(b2, x, y), rings, near=False)
        image = b1_pre + (1 - b1_cov)[..., None] * b2_rgb
    else:
        # One bin: both surfaces' sums added per pixel, nothing in front.
        a = make_layer(strand_color, strand_alpha, strand_r)
        b = make_layer(back_color, back_w, back_r)
        both = a + b
        both[..., 6] = 1.0
        mips = build_mips(both)
        _, _, image = gather(mips, x, y, far_kernel(mips, x, y), rings, near=False)
    m = 14
    sl = (slice(m, SIZE - m), slice(m, SIZE - m))
    return float(np.sqrt(np.mean((image[sl] - truth[sl]) ** 2)))


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

    def test_foreground_over_hidden_background(self):
        # The user's foreground rock: completed reads fill the background the
        # rock hides; the previous reads left the sharp rock under its veil.
        for rings in QUALITY_RINGS:
            new = evaluate_rock(rings, True)
            old = evaluate_rock(rings, False)
            self.assertLess(new, 0.01, rings)
            self.assertLess(new, 0.2 * old, (rings, new, old))
            # A fine pattern hidden behind the rock cannot be recovered;
            # completion still beats the sharp foreground underneath.
            new = evaluate_rock(rings, True, pattern=True)
            old = evaluate_rock(rings, False, pattern=True)
            self.assertLess(new, 0.6 * old, (rings, new, old))

    def test_strands_over_far_background(self):
        # The user's sharp rock between hair strands: one background bin
        # blurs the rock by the strands' radius; B1 over B2 blurs each by
        # its own.
        for rings in QUALITY_RINGS:
            new = evaluate_far_strands(rings, True)
            old = evaluate_far_strands(rings, False)
            self.assertLess(new, 0.03, rings)
            self.assertLess(new, 0.5 * old, (rings, new, old))

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
    print(f"{'rock fringe':16s} {'rings':>5s} {'completed':>10s} {'previous':>10s}")
    for pattern in (False, True):
        for rings in QUALITY_RINGS:
            print(f"{'checker' if pattern else 'smooth':16s} {rings:5d} "
                  f"{evaluate_rock(rings, True, pattern=pattern):10.4f} "
                  f"{evaluate_rock(rings, False, pattern=pattern):10.4f}")
    print(f"{'far strands':16s} {'rings':>5s} {'B1 / B2':>10s} {'one B':>10s}")
    for rings in QUALITY_RINGS:
        print(f"{'':16s} {rings:5d} {evaluate_far_strands(rings, True):10.4f} "
              f"{evaluate_far_strands(rings, False):10.4f}")
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
