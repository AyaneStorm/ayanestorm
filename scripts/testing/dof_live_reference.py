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
  Polygon taps integrate boundary^2 / 2 over their angular sectors,
  including roundness and anamorphic squeeze (aperture_taps()).
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
  recurrence c(l) = S(l) + (1 - V(l)) (S(l) + k c(l + 1)) / (V(l) + k),
  k = 0.05, ending with S / V at the
  top level, evaluated once per texel of each level (build_completed());
  a read is one trilinear fetch. What a bin shows (V = 1) is read exactly; what
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

from dof_reference import aperture_boundary, _blade_cdf, viewer_unit_area

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


def aperture_cdf(angle, blades, roundness):
    """Integral of boundary^2 / 2 from angle 0, continued across blades.
    The same blade primitive defines ASDoFAperture::unitArea()."""
    if blades < 3 or roundness >= 1.0:
        return 0.5 * angle
    half = math.pi / blades
    blade = math.floor(angle / (2.0 * half))
    local = angle - blade * 2.0 * half - half
    return (blade * _blade_cdf(half, blades, roundness) +
            _blade_cdf(local, blades, roundness))


def aperture_taps(rings, shape, midpoint_areas=False):
    """Shader taps including polygon, rotation, squeeze and mip footprint.
    midpoint_areas reproduces the old boundary-at-tap area approximation;
    exact sector integrals partition each annulus without coverage bias."""
    blades, roundness, rotation, anamorphic = shape
    s = 1.0 / (rings + 0.5)
    unit_area = viewer_unit_area(blades, roundness, anamorphic)
    squeeze = max(anamorphic, 1.0)
    taps = [(0.0, 0.0, 0.0, unit_area * 0.25 * s * s, squeeze)]
    for k in range(1, rings + 1):
        count = 6 * k
        offset = 0.5 if k & 1 else 0.0
        half_angle = math.pi / count
        for j in range(count):
            angle = 2.0 * math.pi * (j + offset) / count
            boundary = aperture_boundary(angle, blades, roundness)
            if midpoint_areas:
                sector_area = anamorphic * half_angle * boundary * boundary
            else:
                sector_area = anamorphic * (
                    aperture_cdf(angle + half_angle, blades, roundness) -
                    aperture_cdf(angle - half_angle, blades, roundness))
            a = angle + rotation
            taps.append((anamorphic * math.cos(a) * k * s * boundary,
                         math.sin(a) * k * s * boundary, k * s,
                         2.0 * k * s * s * sector_area, boundary * squeeze))
    return taps, s, unit_area


def live_tap_table(shape, max_rings=7):
    """Mirror of the per-frame tap table of asdoflive.cpp (buildTapTable()):
    per ring tap, in the gather's order, (unit offset x, y, boundary, sector
    area span), float32 as uploaded. Independent of the ring count and of
    the spacing; the gather scales it by d and s."""
    blades, roundness, rotation, anamorphic = shape
    table = []
    for k in range(1, max_rings + 1):
        count = 6 * k
        offset = 0.5 if k & 1 else 0.0
        half_angle = math.pi / count
        for j in range(count):
            angle = 2.0 * math.pi * (j + offset) / count
            boundary = aperture_boundary(angle, blades, roundness)
            span = anamorphic * (aperture_cdf(angle + half_angle, blades, roundness) -
                                 aperture_cdf(angle - half_angle, blades, roundness))
            a = angle + rotation
            table.append((anamorphic * math.cos(a) * boundary, math.sin(a) * boundary,
                          boundary, span))
    return np.array(table, dtype=np.float32)


def taps_from_table(table, rings, anamorphic, unit_area):
    """The gather's taps rebuilt from the table, as asDoFLiveGatherF.glsl
    reads it: offset = xy d, spacing = s z squeeze, area = 2 d s w."""
    s = 1.0 / (rings + 0.5)
    squeeze = max(anamorphic, 1.0)
    taps = [(0.0, 0.0, 0.0, unit_area * 0.25 * s * s, squeeze)]
    index = 0
    for k in range(1, rings + 1):
        d = k * s
        for _ in range(6 * k):
            x, y, boundary, span = (float(v) for v in table[index])
            taps.append((x * d, y * d, d, 2.0 * d * s * span, boundary * squeeze))
            index += 1
    return taps


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


# Weight of the coarser estimate against the level's own visible density
# (asDoFLiveCompleteF.glsl, LIVE_COMPLETE_PRIOR). None: plain push-pull.
COMPLETE_PRIOR = 0.05


def read_completed(mips, x, y, lod, prior=None):
    """The bin alone at lod, visibility-completed by push-pull. The hidden
    part (1 - V) of a level is filled with the visible part's own density
    S / V, blended continuously toward the coarser estimate c(l + 1) as
    less is visible:
        c(l) = S + (1 - V) (S + k c(l + 1)) / (V + k),
    with S / V at the top level. Plain push-pull (k -> infinity),
    c(l) = S + (1 - V) c(l + 1), fills the hidden part from coarser levels
    that average in the empty space around an object: a surface split
    softly between two bins (F 0.6, B1 0.4) read its B1 alone with
    coverage under 1, and the far background leaked through it (the user's
    grey veil in squares).
    The recurrence runs once per texel of each level, top down
    (build_completed(), asDoFLiveCompleteF.glsl), and a read is one trilinear
    fetch of the completed chain. Evaluating it per tap (up to three reads
    per level, about ten levels behind an avatar) gave the same errors
    against brute force or larger (doc, phase 5).
    Returns (values, found); found is false only where no level shows the
    bin."""
    k = COMPLETE_PRIOR if prior is None else prior
    completed = _completed_mips(mips, k)
    out = sample_trilinear(completed, x, y, lod)
    return out, out[..., 3] > 1e-6


_completed_cache = {}


def _completed_mips(mips, k):
    key = (id(mips), k)
    entry = _completed_cache.get(key)
    if entry is None or entry[0] is not mips:
        if len(_completed_cache) > 64:
            _completed_cache.clear()
        entry = (mips, build_completed(mips, k))
        _completed_cache[key] = entry
    return entry[1]


def build_completed(mips, k=COMPLETE_PRIOR):
    """Completed chain of a bin's mips (channels S.rgb, W, E, M):
        c(top) = S / V,
        c(l) = S (1 + k) / (V + k) + k (1 - V) / (V + k) c(l + 1),
    c(l + 1) read bilinearly at level l's texel centres."""
    top = len(mips) - 1
    out = [None] * (top + 1)
    s = mips[top]
    vis = s[..., 6:7]
    out[top] = np.where(vis > 1e-6, s[..., 0:6] / np.maximum(vis, 1e-6), 0.0)
    for level in range(top - 1, -1, -1):
        s = mips[level]
        h, w = s.shape[0], s.shape[1]
        yy, xx = np.mgrid[0:h, 0:w].astype(float)
        coarser = sample_level(out[level + 1], (xx + 0.5) / 2.0, (yy + 0.5) / 2.0)
        vis = s[..., 6:7]
        if k == float("inf"):
            gain, carry = 1.0, 1.0 - vis
        else:
            gain = (1.0 + k) / (vis + k)
            carry = k * (1.0 - vis) / (vis + k)
        out[level] = s[..., 0:6] * gain + carry * coarser
    return out


def read_raw(mips, x, y, lod):
    """The previous read (before completion): the sums divided by V at the
    tap's own level, nothing where the bin is hidden there."""
    v = sample_trilinear(mips, x, y, lod)
    found = v[..., 6] >= 0.001
    out = np.zeros(x.shape + (6,))
    out[found] = v[found][..., 0:6] / v[found][..., 6:7]
    return out, found


def reach_fraction(r, d, s, ring, sa=0.0):
    """Share of a tap's area that a source of radius r reaches. A ring tap
    stands for the annulus [d - s/2, d + s/2], the centre tap for the disc
    of radius s/2; the share is the part of that area inside radius r, so
    the taps integrate pi r^2 exactly for any r (a smoothstep over one
    spacing lost up to 9% coverage for r between rings).

    sa = a sigma: spherical aberration (phase 4 step 2). Light at pupil
    radius rho = t / r of the source's disc weighs 1 - sa (2 rho^2 - 1);
    the share is that profile integrated over the same part, exactly: with
    u = t^2, G(u) = (1 + sa) u - sa u^2 / r^2, and G(r^2) = r^2, so a whole
    disc keeps its energy for any sa (spherical_reach())."""
    if ring:
        inner2 = (d - 0.5 * s) ** 2
        outer2 = (d + 0.5 * s) ** 2
        area = 2.0 * d * s
    else:
        inner2 = 0.0
        outer2 = 0.25 * s * s
        area = 0.25 * s * s
    return spherical_reach(r, inner2, outer2, area, sa)


def spherical_norm(sa):
    """Mean of max(1 + sa - 2 sa u, 0) over u in [0, 1]: 1 for |sa| <= 1.
    Beyond, the profile turns negative near the rim (sa > 1) or the centre
    (sa < -1); it is cut there and divided by this mean, so the source
    keeps its energy (liveSphericalNorm(), sphericalNorm() in modes 1, 2)."""
    sa = np.asarray(sa, float)
    q = (1.0 + sa) ** 2 / (4.0 * np.maximum(np.abs(sa), 1e-12))
    return np.where(np.abs(sa) <= 1.0, 1.0, np.where(sa > 0.0, q, 1.0 + q))


def spherical_cut(sa):
    """(lo, hi) of the profile's positive part in u = rho^2."""
    sa = np.asarray(sa, float)
    cut = (1.0 + sa) / (2.0 * np.where(sa == 0.0, 1.0, sa))
    return np.where(sa < -1.0, cut, 0.0), np.where(sa > 1.0, cut, 1.0)


def spherical_reach(r, inner2, outer2, area, sa):
    r2 = np.maximum(r * r, 1e-12)
    hi = np.minimum(r2, outer2)
    lo = np.minimum(r2, inner2)
    cut_lo, cut_hi = spherical_cut(sa)
    lo = np.maximum(lo, cut_lo * r2)
    hi = np.maximum(np.minimum(hi, cut_hi * r2), lo)
    g = lambda u: (1.0 + sa) * u - sa * u * u / r2
    return np.clip((g(hi) - g(lo)) / area / spherical_norm(sa), 0.0, None)


# Spherical aberration ramps in over this full-resolution blur radius, as in
# the aperture-sampled renderer (SA_FOCUS_PIXELS, asDoFAccumulateF.glsl).
SA_FOCUS_PIXELS = 3.0
# Full-resolution to gather pixels.
GATHER_SCALE = 0.5


def spherical_product(strength, r_gather, near):
    """a sigma of a source of gather-pixel radius r: sigma is the signed
    full-resolution radius / 3, clamped to 1 (negative in front)."""
    sigma = np.minimum(r_gather / GATHER_SCALE / SA_FOCUS_PIXELS, 1.0)
    return strength * (-sigma if near else sigma)


# ---------------------------------------------------------------- cat's eye

# Barrel band nodes per pixel (asDoFLiveGatherF.glsl BARREL_NODES).
BARREL_NODES = 24
# Barrel shift cap, aperture radii (barrelCenter(), asDepthOfFieldSpriteV.glsl).
BARREL_MAX_SHIFT = 1.6


def barrel_center(field, cat_eye):
    shift = cat_eye * np.asarray(field, float)
    n = math.hypot(shift[0], shift[1])
    return shift * (BARREL_MAX_SHIFT / n) if n > BARREL_MAX_SHIFT else shift


def tap_angles(rings):
    """(angle, half sector) of aperture_taps()' taps, in order."""
    out = [(0.0, math.pi)]
    for k in range(1, rings + 1):
        count = 6 * k
        offset = 0.5 if k & 1 else 0.0
        for j in range(count):
            out.append((2.0 * math.pi * (j + offset) / count, math.pi / count))
    return out


def area_to(angle, shape):
    """liveApertureAreaTo(): cumulative unit-aperture area from angle 0."""
    blades, roundness, rotation, anamorphic = shape
    return anamorphic * aperture_cdf(angle, blades, roundness)


def ramp_mean(l0, l1, a, b):
    """Mean over a linear ramp l0..l1 (scalars) of (c, c - c^2), with
    c = clamp(l, a, b) (arrays, a <= b): the moments of the spherical
    profile's antiderivative H(u) = u + sa (u - u^2) along the ramp."""
    a = np.asarray(a, float)
    b = np.maximum(np.asarray(b, float), a)
    lo_v, hi_v = min(l0, l1), max(l0, l1)
    span = hi_v - lo_v
    if span < 1e-9:
        c = np.clip(l0, a, b)
        return c, c - c * c
    pa = np.clip((a - lo_v) / span, 0.0, 1.0)
    pb = np.clip((hi_v - b) / span, 0.0, 1.0)
    pm = np.maximum(1.0 - pa - pb, 0.0)
    x0 = np.maximum(lo_v, a)
    x1 = np.maximum(np.minimum(hi_v, b), x0)
    mean = 0.5 * (x0 + x1)
    sq = (x0 * x0 + x0 * x1 + x1 * x1) / 3.0
    return (pa * a + pb * b + pm * mean,
            pa * (a - a * a) + pb * (b - b * b) + pm * (mean - sq))


class BarrelBand:
    """Cat's eye (phase 4 step 3). A pupil point p of a source's unit
    aperture (rotation, polygon and squeeze included) passes when
    |p - barrel| <= 1. In the area-uniform angle A = area_to(theta) and
    u = tau^2 (tau = pupil radius / boundary), every aperture is the
    rectangle [0, U] x [0, 1], every tap an exact sub-rectangle, and the
    open part a band u1(A) <= u <= u2(A) along each ray. The band is
    sampled at BARREL_NODES angles and linear in A between them; taps and
    the per-pixel open fraction integrate that same band exactly, so a
    uniform field keeps coverage 1 for any kernel, ring count, source
    radius and shape. Only the barrel's outline is approximated."""

    def __init__(self, barrel, shape, nodes=BARREL_NODES):
        self.n = nodes
        self.shape = shape
        self.lo = np.zeros(nodes)
        self.hi = np.zeros(nodes)
        self.area = np.zeros(nodes + 1)
        blades, roundness, rotation, anamorphic = shape
        barrel = np.asarray(barrel, float)
        for j in range(nodes + 1):
            theta = 2.0 * math.pi * j / nodes
            self.area[j] = area_to(theta, shape)
            if j == nodes:
                break
            boundary = aperture_boundary(theta, blades, roundness)
            a = theta + rotation
            v = np.array([math.cos(a) * anamorphic, math.sin(a)]) * boundary
            qa = v @ v
            qb = v @ barrel
            qc = barrel @ barrel - 1.0
            disc = qb * qb - qa * qc
            if disc < 0.0:
                # The ray misses the barrel: an empty band, continuous
                # with the tangent ray's.
                t1 = t2 = qb / qa
            else:
                q = math.sqrt(disc)
                t1, t2 = (qb - q) / qa, (qb + q) / qa
            self.lo[j] = max(t1, 0.0) ** 2
            self.hi[j] = max(t2, 0.0) ** 2
        self.U = self.area[nodes] - self.area[0]
        f0, f1 = self.integral(0.0, self.U, 0.0, 2.0 * math.pi, 0.0, 1.0)
        self.fraction = (float(f0) / self.U, float(f1) / self.U)

    def integral(self, a0, a1, theta0, theta1, ua, ub):
        """Moments (open, profile) of the band over A in [a0, a1] (the
        sector theta0..theta1) and u in [ua, ub]."""
        n = self.n
        step = 2.0 * math.pi / n
        m0 = np.zeros(np.shape(ua))
        m1 = np.zeros(np.shape(ua))
        for j in range(math.floor(theta0 / step), math.floor(theta1 / step) + 1):
            wraps = math.floor(j / n)
            jj = j - wraps * n
            k = (jj + 1) % n
            s0 = self.area[jj] + wraps * self.U
            s1 = self.area[jj + 1] + wraps * self.U
            x0, x1 = max(s0, a0), min(s1, a1)
            if x1 <= x0:
                continue
            t0, t1 = (x0 - s0) / (s1 - s0), (x1 - s0) / (s1 - s0)
            h0, h1 = ramp_mean(self.hi[jj] + (self.hi[k] - self.hi[jj]) * t0,
                               self.hi[jj] + (self.hi[k] - self.hi[jj]) * t1, ua, ub)
            l0, l1 = ramp_mean(self.lo[jj] + (self.lo[k] - self.lo[jj]) * t0,
                               self.lo[jj] + (self.lo[k] - self.lo[jj]) * t1, ua, ub)
            m0 = m0 + (x1 - x0) * (h0 - l0)
            m1 = m1 + (x1 - x0) * (h1 - l1)
        return m0, m1

    def reach(self, r, d, s, ring, sa, theta, half):
        """reach_fraction() with the barrel, compensated: the share of the
        tap's area the source reaches through the barrel, over the open
        fraction of its aperture (same band)."""
        if ring:
            lo, hi = (d - 0.5 * s) ** 2, (d + 0.5 * s) ** 2
        else:
            lo, hi = 0.0 * s, 0.25 * s * s
        r2 = np.maximum(r * r, 1e-12)
        cut_lo, cut_hi = spherical_cut(sa)
        ua = np.maximum(np.minimum(lo / r2, 1.0), cut_lo)
        ub = np.maximum(np.minimum(np.minimum(hi / r2, 1.0), cut_hi), ua)
        a0 = area_to(theta - half, self.shape)
        a1 = area_to(theta + half, self.shape)
        m0, m1 = self.integral(a0, a1, theta - half, theta + half, ua, ub)
        # Beyond |sa| = 1 the open fraction is the cut profile's.
        w0, w1 = self.integral(0.0, self.U, 0.0, 2.0 * math.pi, cut_lo, cut_hi)
        f = np.where(np.abs(sa) > 1.0, (w0 + sa * w1) / self.U,
                     self.fraction[0] + sa * self.fraction[1])
        f = np.maximum(f, 0.01)
        return np.maximum(m0 + sa * m1, 0.0) / ((a1 - a0) * (hi - lo) / r2) / f


# Axial chromatic aberration (phase 4 step 4), the aperture-sampled
# renderer's spectral model (ASDoFAperture::spectralWeights): wavelength s blurs to radius r - sigma delta s
# (sigma +1 behind the focus, -1 in front), four strata of s, channel
# weights red 1 + s, green 1.5 (1 - s^2), blue 1 - s, each summing to 1.
CA_STRATA = np.array([-0.75, -0.25, 0.25, 0.75])
CA_WEIGHTS = np.array([[0.0625, 0.1875, 0.3125, 0.4375],
                       [0.1590909, 0.3409091, 0.3409091, 0.1590909],
                       [0.4375, 0.3125, 0.1875, 0.0625]])


def ca_strata(ca_delta, near):
    """(radius offset, rgb weights) per stratum: r_k = |r + offset|, floored
    at half a gather pixel like every source. One neutral stratum when off."""
    if ca_delta <= 0.0:
        return [(0.0, np.ones(3))]
    sigma = -1.0 if near else 1.0
    return [(-sigma * ca_delta * CA_STRATA[k], CA_WEIGHTS[:, k]) for k in range(4)]


def stratum_radius(r, offset):
    return r if offset == 0.0 else np.maximum(np.abs(r + offset), 0.5)


def gather(mips, x, y, kernel_radius, rings, near, complete=True,
           shape=(0, 1.0, 0.0, 1.0), midpoint_areas=False, sa_strength=0.0,
           barrel=None, self_fill=False, ca_delta=0.0):
    """Area-tap scatter-as-gather at pixels (x, y) (pixel centres).

    Returns (premultiplied rgb, coverage, rgb). kernel_radius: array per
    pixel. near: True for one shared kernel (coverage is the energy sum),
    False for the far layer (colour normalized, coverage clamped energy sum).

    self_fill (B1, the near background): behind the focus, sharper content
    is nearer. A tap whose B1 is sharper than this pixel's own hides B1
    behind it that no bin stores (only nearer bins trigger completion): the
    jaw edge, partly B1 on the focus ramp, over a more blurred B1 neck left
    a light line below the jaw. Each tap's hidden share is what content like
    the pixel's own B1 (density W_p, radius r_p) would add over the part of
    the tap its own content does not reach; the coverage deficit is filled
    with the pixel's colour, up to that sum. Equal radii (and smooth ramps,
    whose coverage is already 1) add nothing; neither do taps without B1
    (a true edge over the far background) (evaluate_self_occlusion()).

    ca_delta > 0 (axial CA, gather pixels): every tap evaluates the four
    strata, each with its own radius, its own exact reach and its energy
    rescaled (W / r_k^2, so each stratum's disc keeps the source's
    energy), weighted per channel; coverage is then per channel (an
    (..., 3) array). kernel_radius must already include 0.75 ca_delta (the
    widest stratum), as the tile pass adds it."""
    taps, s_unit, unit_area = aperture_taps(rings, shape, midpoint_areas)
    R = np.maximum(kernel_radius, 0.5)
    s = s_unit * R
    strata = ca_strata(ca_delta, near)
    color = np.zeros(x.shape + (3,))
    weight = np.zeros(x.shape + (3,))
    band = BarrelBand(barrel, shape) if barrel is not None else None

    def tap_reach(r, sa, d, ring, theta, half):
        if band is None:
            return reach_fraction(r, d, s, ring, sa)
        return band.reach(r, d, s, ring, sa, theta, half)

    if self_fill:
        vp, fp = read_completed(mips, x, y, np.zeros(x.shape))
        w_p = np.where(fp, vp[..., 3], 0.0)
        r_p = np.sqrt(w_p / np.maximum(np.where(fp, vp[..., 4], 0.0), 1e-12))
        c_p = vp[..., 0:3] / np.maximum(w_p, 1e-12)[..., None]
        own = w_p > 0.001
        hidden = np.zeros(x.shape + (3,))
    for (dx, dy, d_unit, area_unit, footprint), (theta, half) in zip(taps, tap_angles(rings)):
        lod = np.log2(np.maximum(LOD_SCALE * s * footprint, 1.0))
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
        rgb = v[..., 0:3] / np.maximum(W, 1e-12)[..., None]
        for offset, channels in strata:
            r_k = stratum_radius(r_tap, offset)
            e_k = E if offset == 0.0 else W / (r_k * r_k)
            reach = tap_reach(r_k, spherical_product(sa_strength, r_k, near), d, d_unit > 0,
                              theta, half)
            wt = (area / unit_area * e_k * reach)[..., None] * channels
            color += rgb * wt
            weight += wt
            if self_fill:
                rp_k = stratum_radius(r_p, offset)
                reach_p = tap_reach(rp_k, spherical_product(sa_strength, rp_k, near), d,
                                    d_unit > 0, theta, half)
                sharper = own & (W > 0.0) & (r_k < rp_k)
                hidden += np.where(sharper, area / unit_area * W * w_p /
                                   np.maximum(rp_k * rp_k, 1e-12) *
                                   np.maximum(reach_p - reach, 0.0), 0.0)[..., None] * channels
    coverage = np.clip(weight, 0.0, 1.0)
    rgb = color / np.maximum(weight, 1e-12)
    pre = rgb * coverage
    if self_fill:
        fill = np.minimum(1.0 - coverage, hidden)
        pre = pre + c_p * fill
        coverage = coverage + fill
        rgb = pre / np.maximum(coverage, 1e-12)
    if ca_delta <= 0.0:
        coverage = coverage[..., 1]
    return pre, coverage, rgb


# ---------------------------------------------------------------- truth

def scatter_truth(color, alpha, radius, sa_strength=0.0, near=True, barrel=None):
    """Brute-force splat: each source spreads alpha over its own disc
    (1 px antialiased edge), normalized to the disc's area. With spherical
    aberration, every pixel of the disc also weighs 1 - a sigma (2 rho^2 - 1),
    rho = distance / r, renormalized to keep the source's energy (the
    antialiased edge reaches past rho = 1). With a barrel (circular
    aperture), a disc pixel passes where its pupil point, (source - pixel)
    / r in front of the focus and (pixel - source) / r behind, lies inside
    the barrel (1 px antialiased), renormalized likewise (compensated)."""
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
        if sa_strength != 0.0:
            sa = float(spherical_product(sa_strength, r, near))
            rho2 = np.minimum(dist * dist / (r * r), 1.0)
            profiled = k * np.maximum(1.0 - sa * (2.0 * rho2 - 1.0), 0.0)
            k = profiled * k.sum() / profiled.sum()
        if barrel is not None:
            sign = 1.0 if near else -1.0
            qx = sign * (px - xx[y0:y1, x0:x1]) / r - barrel[0]
            qy = sign * (py - yy[y0:y1, x0:x1]) / r - barrel[1]
            inside = np.clip((1.0 - np.hypot(qx, qy)) * r + 0.5, 0.0, 1.0)
            clipped = k * inside
            k = clipped * k.sum() / max(clipped.sum(), 1e-12)
        # Normalize by the full disc area (also outside the frame).
        k = k * alpha[py, px] / (math.pi * r * r)
        acc[y0:y1, x0:x1] += color[py, px] * k[..., None]
        cov[y0:y1, x0:x1] += k
    coverage = np.clip(cov, 0.0, 1.0)
    rgb = acc / np.maximum(cov, 1e-12)[..., None]
    return rgb * coverage[..., None], coverage, rgb, cov


def scatter_truth_ca(color, alpha, radius, near, delta):
    """scatter_truth() per wavelength stratum: every source splats each
    stratum's disc (radius as ca_strata()) into the channels by their
    weights. Returns per-channel (premultiplied rgb, coverage)."""
    h, w = alpha.shape
    acc = np.zeros((h, w, 3))
    cov = np.zeros((h, w, 3))
    yy, xx = np.mgrid[0:h, 0:w]
    ys, xs = np.nonzero(alpha > 0)
    for py, px in zip(ys, xs):
        for offset, channels in ca_strata(delta, near):
            r = float(stratum_radius(max(radius[py, px], 0.5), offset))
            ext = int(math.ceil(r + 1))
            y0, y1 = max(py - ext, 0), min(py + ext + 1, h)
            x0, x1 = max(px - ext, 0), min(px + ext + 1, w)
            dist = np.hypot(xx[y0:y1, x0:x1] - px, yy[y0:y1, x0:x1] - py)
            k = np.clip(r + 0.5 - dist, 0.0, 1.0) * alpha[py, px] / (math.pi * r * r)
            acc[y0:y1, x0:x1] += k[..., None] * channels * color[py, px]
            cov[y0:y1, x0:x1] += k[..., None] * channels
    coverage = np.clip(cov, 0.0, 1.0)
    return acc / np.maximum(cov, 1e-12) * coverage, coverage


def evaluate_ca_edge(rings, delta_ratio, front, back, near=True, R=10.0):
    """A blurred half-plane (radius R, axial CA delta = delta_ratio R) over
    a uniform background. Returns, over the fringe band, the rms of (the
    per-channel gather composited per channel - the per-channel truth),
    of (single green alpha - per channel), and of the true fringe colour
    (each pixel's deviation from its grey)."""
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    sel = xx < SIZE / 2
    color = np.zeros((SIZE, SIZE, 3))
    color[sel] = front
    alpha = sel.astype(float)
    radius = np.where(sel, R, 0.0)
    delta = delta_ratio * R
    mips = build_mips(make_layer(color, alpha, radius))
    kernel = tile_kernel(radius, alpha) + 0.75 * delta
    pre, cov, _ = gather(mips, xx + 0.5, yy + 0.5, kernel, rings, near=near, ca_delta=delta)
    t_pre, t_cov = scatter_truth_ca(color, alpha, radius, near, delta)
    bg = np.array(back, float)
    per_channel = pre + (1 - cov) * bg
    single = pre + (1 - cov[..., 1:2]) * bg
    truth = t_pre + (1 - t_cov) * bg
    band = (np.abs(xx + 0.5 - SIZE / 2) < R + delta) & (yy > 20) & (yy < SIZE - 20)
    rms = lambda a: float(np.sqrt(np.mean(a[band] ** 2)))
    fringe = truth - truth.mean(axis=-1, keepdims=True)
    return rms(per_channel - truth), rms(single - per_channel), rms(fringe)


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


def completed_radius(mips):
    """Per-pixel (radius, weight) of a layer as the gathers read it:
    completed (read_completed()). The tile pass reduces this, not the raw
    level 0: the gathers read filled-in content where nearer bins hide the
    bin, and a tile kernel of 0 there skipped that fill tile by tile."""
    h, w = mips[0].shape[0], mips[0].shape[1]
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    v, found = read_completed(mips, xx + 0.5, yy + 0.5, np.zeros((h, w)))
    weight = np.where(found, v[..., 3], 0.0)
    radius = np.sqrt(weight / np.maximum(v[..., 4], 1e-12))
    return radius, np.where(weight > 0.001, 1.0, 0.0)


def evaluate(name, rings, background=(0.5, 0.5, 0.5), sa_strength=0.0, near=True,
             shape=(0, 1.0, 0.0, 1.0), barrel=None):
    color, alpha, radius = scene(name)
    t_pre, t_cov, _, t_energy = scatter_truth(color, alpha, radius, sa_strength, near,
                                              barrel)
    mips = build_mips(make_layer(color, alpha, radius))
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    R = tile_kernel(radius, alpha)
    g_pre, g_cov, _ = gather(mips, xx + 0.5, yy + 0.5, R, rings, near=near,
                             shape=shape, sa_strength=sa_strength, barrel=barrel)
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
                                   near=False, self_fill=True)
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


def evaluate_self_occlusion(self_fill, max_blur_pct=3.0, rings=7, shape=(6, 0.0, 0.0, 1.0)):
    """The user's jaw line (1398 px high, max blur 3%, multipliers 1,
    Cinematic, hexagon): an in-focus face whose edge is blurred 1.2 px (F
    0.55, B1 0.45 on the focus ramp) above a neck blurred 6.5 px (B1), a
    bright far background (B2) at the left. Gather resolution is half.
    Returns (largest colour error on the neck below the jaw, B1 coverage
    across the neck / background edge)."""
    def ramp(e0, e1, v):
        t = np.clip((v - e0) / (e1 - e0), 0.0, 1.0)
        return t * t * (3 - 2 * t)
    max_coc = 0.01 * max_blur_pct * 1398
    far_split = max(math.sqrt(2.0 * max_coc), 2.5)
    yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    face = yy < 48
    beach = xx < 16
    r_full = np.where(beach, min(40.0, max_coc), np.where(face, 1.2, 6.5))
    color = np.zeros((SIZE, SIZE, 3))
    color[:] = (0.80, 0.55, 0.42)
    color[face] = (0.85, 0.60, 0.47)
    color[beach] = (0.95, 0.95, 0.92)
    focus = 1.0 - ramp(0.5, 2.0, r_full)
    strong = (1.0 - focus) * ramp(0.8 * far_split, 1.25 * far_split, r_full)
    b1 = 1.0 - focus - strong
    r_g = np.maximum(0.5 * r_full, 0.5)
    m1 = build_mips(make_layer(color, b1, r_g, visibility=1.0 - focus))
    m2 = build_mips(make_layer(color, strong, r_g, visibility=1.0 - focus - b1))
    x, y = xx + 0.5, yy + 0.5
    r_tiles, a_tiles = completed_radius(m1)
    b1_pre, b1_cov, _ = gather(m1, x, y, tile_kernel(r_tiles, a_tiles), rings, near=False,
                               shape=shape, self_fill=self_fill)
    _, _, b2_rgb = gather(m2, x, y, far_kernel(m2, x, y), rings, near=False, shape=shape)
    back = b1_pre + (1 - b1_cov)[..., None] * b2_rgb
    image = focus[..., None] * color + (1 - focus)[..., None] * back
    neck = (slice(48, 56), slice(24, 88))
    return float(np.abs(image[neck] - color[neck]).max()), b1_cov[70, 14:21]


def evaluate_split_surface(prior):
    """An object split softly between two bins (F 0.6, B1 0.4, as on the
    0.5-2 px ramp) in front of empty space. Its B1 alone must stay opaque
    inside the object: the hidden 0.6 is the same surface. Returns the
    largest coverage deficit inside the object, away from its edge."""
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    inside = (np.abs(xx - w / 2) < 20) & (np.abs(yy - h / 2) < 30)
    color = np.zeros((h, w, 3))
    color[:] = (0.6, 0.4, 0.3)
    w_b1 = np.where(inside, 0.4, 0.0)
    vis = np.where(inside, 0.4, 1.0)
    mips = build_mips(make_layer(color, w_b1, np.full((h, w), 1.0), visibility=vis))
    core = (np.abs(xx - w / 2) < 17) & (np.abs(yy - h / 2) < 27)
    v, _ = read_completed(mips, xx[core] + 0.5, yy[core] + 0.5, np.zeros(int(core.sum())),
                          prior=prior)
    return float(np.max(1.0 - v[..., 3]))


def evaluate_hidden_veil_tiles(rings, completed):
    """A veil bin (B1, radius 2) visible on the left and hidden by an
    opaque in-focus surface on the right (V = 0): what the gather reads
    there is the completed fill. Returns the largest coverage step between
    neighbouring pixels inside the hidden part (the user's grey blocks
    behind the in-focus torso). Tile kernels from the raw bin are 0 past
    the dilation reach, and the fill stops at a tile edge; from the
    completed bin the fill is continuous."""
    h = w = SIZE
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    color = np.zeros((h, w, 3))
    color[:] = (0.5, 0.3, 0.2)
    visible = xx < 30
    alpha = np.where(visible, 1.0, 0.0)
    radius = np.full((h, w), 2.0)
    mips = build_mips(make_layer(color, alpha, radius, visibility=visible.astype(float)))
    if completed:
        r_tiles, a_tiles = completed_radius(mips)
    else:
        r_tiles, a_tiles = radius, alpha
    kernel = tile_kernel(r_tiles, a_tiles)
    _, cov, _ = gather(mips, xx + 0.5, yy + 0.5, kernel, rings, near=False, self_fill=True)
    # The gathers skip tiles that hold nothing (asDoFLiveGatherF.glsl).
    cov = np.where(kernel > 0.0, cov, 0.0)
    hidden = cov[16:h - 16, 34:w - 4]
    return float(np.abs(np.diff(hidden, axis=1)).max())


def evaluate_polygon_tiles(midpoint_areas):
    """Opaque B1 over a hidden light B2, with actual stepped tile maxima.
    A larger source changes nearby tiles' kernels. The measured strip is
    outside that source's reach, so its B1 coverage must remain one. Ring
    counts adapt to each kernel as in asDoFLiveGatherF.glsl."""
    h = w = 64
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    color = np.empty((h, w, 3))
    color[:] = (0.12, 0.06, 0.03)
    weight = np.ones((h, w))
    radius = np.full((h, w), 0.5)
    radius[8, 40] = 1.5
    mips = build_mips(make_layer(color, weight, radius))
    kernel = tile_kernel(radius, weight)
    ring_counts = np.clip(np.ceil(kernel - 0.5), 1, 7).astype(int)
    coverage = np.zeros((h, w))
    premultiplied = np.zeros((h, w, 3))
    for rings in np.unique(ring_counts):
        selected = ring_counts == rings
        pre, cov, _ = gather(mips, xx[selected] + 0.5, yy[selected] + 0.5,
                             kernel[selected], int(rings), near=False,
                             shape=(6, 0.0, 0.0, 1.0), midpoint_areas=midpoint_areas,
                             self_fill=True)
        coverage[selected] = cov
        premultiplied[selected] = pre
    image = premultiplied + (1.0 - coverage)[..., None] * np.array((0.6, 0.6, 0.6))
    strip = (slice(16, 20), slice(8, 60))
    return coverage[strip], image[strip]


# ---------------------------------------------------------------- highlight sprites
#
# Phase 3: isolated defocused lights leave the gather input and are drawn as
# aperture sprites (asDepthOfFieldHighlightF.glsl and asDepthOfFieldSpriteV/F
# .glsl). A source smaller than the tap spacing is seen only
# through the taps whose mip footprint covers it, so its bokeh carries a ring
# pattern (about 25% relative variation inside a triangle at any ring count).

SPRITE_CA_STRATA = (-0.75, -0.25, 0.25, 0.75)
SPRITE_CA_WEIGHTS = np.array([[0.0625, 0.1875, 0.3125, 0.4375],
                              [0.1590909, 0.3409091, 0.3409091, 0.1590909],
                              [0.4375, 0.3125, 0.1875, 0.0625]])
# Antialiasing grids of asDepthOfFieldSpriteF.glsl (target-pixel units).
SPRITE_GRID4 = ((0.125, 0.375), (-0.375, 0.125), (-0.125, -0.375), (0.375, -0.125))
SPRITE_GRID16 = tuple(((i + 0.5) / 4 - 0.5 + ((j % 2) - 0.5) * 0.125,
                       (j + 0.5) / 4 - 0.5 + ((i % 2) - 0.5) * 0.125)
                      for j in range(4) for i in range(4))
SPRITE_GRID64 = tuple(((i + 0.5) / 8 - 0.5, (j + 0.5) / 8 - 0.5) for j in range(8) for i in range(8))


def sprite_boundary(phi, blades, roundness):
    """boundary() of asDepthOfFieldSpriteF.glsl: edge radius and the radial
    to perpendicular gap scale."""
    if blades < 3:
        return np.ones_like(phi), np.ones_like(phi)
    sector = 2 * math.pi / blades
    local = np.mod(phi, sector) - 0.5 * sector
    polygon = math.cos(0.5 * sector) / np.maximum(np.cos(local), 0.001)
    return (polygon * (1 - roundness) + roundness,
            np.cos(local) * (1 - roundness) + roundness)


def sprite_edge(qx, qy, radius, plane, shape, barrel):
    """liveEdge() (asDepthOfFieldSpriteF.glsl): signed distance inside the aperture of this
    radius, barrel included (the nearer edge wins), full-resolution pixels,
    and the pupil position over the edge radius. No astigmatism in Live."""
    blades, roundness, rotation, anamorphic = shape
    sign = 1.0 if plane > 0 else -1.0
    ux, uy = qx * sign / radius, qy * sign / radius
    vx, vy = ux / anamorphic, uy
    r = np.hypot(vx, vy)
    b, edge_scale = sprite_boundary(np.arctan2(vy, vx) - rotation, blades, roundness)
    edge = (b - r) * edge_scale * radius
    if barrel is not None:
        edge = np.minimum(edge, (1.0 - np.hypot(ux - barrel[0], uy - barrel[1])) * radius)
    return edge, r / np.maximum(b, 1e-4)


def sprite_profile(rho, c):
    """sphericalWeight() at relative pupil radius rho, c = a sigma, cut at
    zero and divided by its mean (spherical_norm())."""
    if c == 0.0:
        return np.ones_like(rho)
    return np.maximum(1.0 - c * (2.0 * np.clip(rho * rho, 0.0, 1.0) - 1.0), 0.0) / \
        float(spherical_norm(c))


def sprite_profile_moment(c, lo, hi):
    """Integral of the cut profile times rho over [lo, hi]."""
    if c > 1.0:
        hi = min(hi, math.sqrt((1.0 + c) / (2.0 * c)))
    elif c < -1.0:
        lo = max(lo, math.sqrt((1.0 + c) / (2.0 * c)))
    if hi <= lo:
        return 0.0
    g = lambda x: 0.5 * (1.0 + c) * x * x - 0.5 * c * x ** 4
    return (g(hi) - g(lo)) / float(spherical_norm(c))


def sprite_open_fraction(shape, barrel, c, angles=64):
    """liveOpenFraction() (asDepthOfFieldSpriteV.glsl): the
    share of the profiled aperture inside the barrel, a polar integral over
    the aperture angle with the exact ray and circle intersection. Mode 1
    divides by the circle's vesica fraction, which is off by up to 58% for a
    clipped triangle and ignores the spherical profile's cut."""
    if barrel is None:
        return 1.0
    blades, roundness, rotation, anamorphic = shape
    inside = full = 0.0
    for i in range(angles):
        phi = 2.0 * math.pi * (i + 0.5) / angles
        b = float(sprite_boundary(np.array(phi), blades, roundness)[0])
        dx, dy = anamorphic * math.cos(phi + rotation), math.sin(phi + rotation)
        qa = dx * dx + dy * dy
        qb = -2.0 * (dx * barrel[0] + dy * barrel[1])
        qc = barrel[0] ** 2 + barrel[1] ** 2 - 1.0
        disc = qb * qb - 4.0 * qa * qc
        if disc > 0.0:
            root = math.sqrt(disc)
            lo = max((-qb - root) / (2.0 * qa), 0.0)
            hi = min((-qb + root) / (2.0 * qa), b)
            inside += b * b * sprite_profile_moment(c, lo / b, hi / b)
        full += b * b * sprite_profile_moment(c, 0.0, 1.0)
    return inside / full


def live_sprite(gx, gy, cx, cy, radius, plane, shape, energy, barrel=None, delta=0.0,
                sa_strength=0.0, scale=2.0):
    """Mirror of the Live sprite (asDepthOfFieldSpriteV/F.glsl)
    at target pixels (gx, gy) (indices), centre (cx, cy) in full-resolution
    pixels: per-channel radiance per full-resolution pixel. Every axial CA
    stratum is an exact scaled aperture with its own antialiasing grid and
    keeps its energy; the open fraction is exact (sprite_open_fraction())."""
    radius = max(radius, 1.0)
    unit_area = viewer_unit_area(shape[0], shape[1], shape[3])
    sign = 1.0 if plane > 0 else -1.0
    c = sa_strength * sign * min(radius / 3.0, 1.0)
    fraction = max(sprite_open_fraction(shape, barrel, c), 0.01)
    strata = ([(0.0, np.ones(3))] if delta <= 0.01 else
              [(SPRITE_CA_STRATA[k], SPRITE_CA_WEIGHTS[:, k]) for k in range(4)])
    px, py = (gx + 0.5) * scale, (gy + 0.5) * scale
    out = np.zeros(np.shape(gx) + (3,))
    for offset, channels in strata:
        r_k = max(radius - sign * delta * offset, 1.0)
        # Sub-pixel features (a 3 px triangle, a thin spherical ring) need
        # the finer grids: 16 samples left up to 23% energy error there.
        if r_k < 3.0 * scale:
            grid, aa = SPRITE_GRID64, 0.125 * scale
        elif r_k < 6.0 * scale:
            grid, aa = SPRITE_GRID16, 0.25 * scale
        elif r_k < 12.0 * scale:
            grid, aa = SPRITE_GRID4, 0.5 * scale
        else:
            grid, aa = ((0.0, 0.0),), scale
        cover = 0.0
        for ox, oy in grid:
            edge, rho = sprite_edge(px + ox * scale - cx, py + oy * scale - cy, r_k, plane,
                                    shape, barrel)
            cover = cover + np.clip(edge / aa + 0.5, 0.0, 1.0) * sprite_profile(rho, c)
        cover = cover / len(grid)
        out += (cover / (unit_area * r_k * r_k * fraction))[..., None] * channels
    return np.asarray(energy, dtype=float) * out


def ideal_bokeh(gx, gy, cx, cy, radius, plane, shape, energy, barrel=None, delta=0.0,
                sa_strength=0.0, scale=2.0, supersample=8):
    """Truth: a point light's bokeh averaged over each target pixel, per
    stratum the clipped aperture with the spherical profile, normalized to
    keep the energy (compensated, as the gathers)."""
    blades, roundness, rotation, anamorphic = shape
    sign = 1.0 if plane > 0 else -1.0
    c = sa_strength * sign * min(max(radius, 1.0) / 3.0, 1.0)
    offsets = (np.arange(supersample) + 0.5) / supersample
    strata = ([(0.0, np.ones(3))] if delta <= 0.01 else
              [(SPRITE_CA_STRATA[k], SPRITE_CA_WEIGHTS[:, k]) for k in range(4)])
    out = np.zeros(np.shape(gx) + (3,))
    for offset, channels in strata:
        r_k = max(radius - sign * delta * offset, 1.0)
        acc = np.zeros(np.shape(gx))
        for oy in offsets:
            for ox in offsets:
                ux = ((gx + ox) * scale - cx) * sign / r_k
                uy = ((gy + oy) * scale - cy) * sign / r_k
                r = np.hypot(ux / anamorphic, uy)
                b, _ = sprite_boundary(np.arctan2(uy, ux / anamorphic) - rotation, blades,
                                       roundness)
                weight = (r <= b) * sprite_profile(r / b, c)
                if barrel is not None:
                    weight = weight * (np.hypot(ux - barrel[0], uy - barrel[1]) <= 1.0)
                acc += weight
        total = acc.sum() * scale * scale
        out += (acc / max(total, 1e-12))[..., None] * channels
    return np.asarray(energy, dtype=float) * out


def live_highlight_gain(strength, threshold, luminance, radius):
    """highlightGain() (asDepthOfFieldHighlightF.glsl): the Aperture-sampled renderer's bright
    bokeh highlights (asDoFAccumulateF.glsl, artistic, not energy
    preserving) on the sprites' light: 1 + strength * bright * area, area =
    min((radius / 4)^2, 1024) with the full-resolution blur radius. The
    extraction has already found the light isolated, so mode 2's ring test
    is left out; strength 0 is off."""
    if strength <= 0.0 or radius <= 1.0:
        return 1.0
    bright = float(smoothstep(0.5 * threshold, 1.5 * threshold, luminance))
    return 1.0 + strength * bright * min((radius / 4.0) ** 2, 1024.0)


def extract_highlights(image, radius, isolation=2.0, budget=4096, boost=None):
    """asDepthOfFieldHighlightF.glsl at full resolution: the gather input
    (image minus the excess of kept cells) and the kept cells' sprites
    (energy, centroid x, y, signed radius). boost (strength, threshold):
    the bright highlights gain on the cells' energy only; the gather input
    loses the light itself."""
    from dof_reference import viewer_highlight_detect, viewer_highlight_keep_cells
    h, w = radius.shape
    magnitude = np.abs(radius)
    excess = np.zeros_like(image)
    for y in range(h):
        for x in range(w):
            if magnitude[y, x] > 2.0:
                excess[y, x] = viewer_highlight_detect(image, magnitude, x, y, isolation)
    lum = excess @ np.array([0.2126, 0.7152, 0.0722])
    boosted = excess
    if boost is not None:
        pixel_lum = image @ np.array([0.2126, 0.7152, 0.0722])
        gain = np.vectorize(lambda l, r: live_highlight_gain(boost[0], boost[1], l, r))(
            pixel_lum, magnitude)
        boosted = excess * gain[..., None]
    cells = {}
    for cy in range(0, h, 8):
        for cx in range(0, w, 8):
            l = lum[cy:cy + 8, cx:cx + 8]
            if l.sum() <= 0.0001:
                continue
            yy, xx = np.mgrid[cy:cy + l.shape[0], cx:cx + l.shape[1]]
            cells[(cx // 8, cy // 8)] = (
                boosted[cy:cy + 8, cx:cx + 8].sum(axis=(0, 1)),
                float(((xx + 0.5) * l).sum() / l.sum()), float(((yy + 0.5) * l).sum() / l.sum()),
                float((radius[cy:cy + 8, cx:cx + 8] * l).sum() / l.sum()))
    kept = viewer_highlight_keep_cells(
        {cell: float(v[0] @ np.array([0.2126, 0.7152, 0.0722])) for cell, v in cells.items()},
        budget)
    gather_input = image.copy()
    for (cx, cy) in kept:
        gather_input[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] -= excess[cy * 8:cy * 8 + 8,
                                                                     cx * 8:cx * 8 + 8]
    return gather_input, [cells[cell] for cell in kept], excess


def sprite_layer_parts(front_share, back_share, transmittance, v_front, v_back):
    """highlightParts() (asDepthOfFieldHighlightF.glsl): the light alone in the front and the
    back bin of its side (B1 and B2 behind the focus, N2 and N1 in front),
    a_b = share_b T / V_b. The front part is drawn over its layer pair, the
    back part into the back layer, under what the front holds there."""
    a_front = min(front_share * transmittance / max(v_front, 1e-4), 1.0)
    a_back = min(back_share * transmittance / max(v_back, 1e-4), 1.0)
    return a_front, a_back


def sprite_far_visibility(gx, gy, cx, cy, radius, shape, kernel, scale=2.0):
    """The far (B2) sprite at each target pixel only where that pixel's own
    B2 kernel (completed M / W, gather pixels) reaches the light, as the far
    gather sees it: a nearer, sharper B2 surface (palm leaves over a lit
    backdrop) hides the spread of a farther light. Full up to half a pixel
    past the kernel (the sprite's antialiased rim), then over one pixel."""
    qx = (gx + 0.5) * scale - cx
    qy = (gy + 0.5) * scale - cy
    b, _ = sprite_boundary(np.arctan2(qy, qx / shape[3]) - shape[2], shape[0], shape[1])
    distance = np.hypot(qx / shape[3], qy) / b / scale
    return np.clip(kernel - distance + 1.5, 0.0, 1.0)


def sprite_layer_scale(front_share, back_share, transmittance, v_front, v_back):
    """Former highlightScale(), superseded by highlightParts(): the opaque light's energy in the
    layer the sprite is drawn into, the background (B1 over B2) or the veil
    (N2 over N1). Each bin is read alone (S / V), so the surface's share b
    shows a_b = share_b T / V_b of its light, and the pair is composited
    over: a_front + (1 - a_front) a_back. The composite then attenuates the
    layer by whatever it lays over it."""
    a_front = min(front_share * transmittance / max(v_front, 1e-4), 1.0)
    a_back = min(back_share * transmittance / max(v_back, 1e-4), 1.0)
    return a_front + (1.0 - a_front) * a_back


def evaluate_sprite_light(rings, shape, sprites=True, size=128, radius_full=20.0):
    """A small bright light over a dim textured far background (one radius),
    through the Live far gather with and without sprites, at gather
    resolution. Returns (light contribution, ideal light bokeh, mask of the
    ideal interior, light energy)."""
    rng = np.random.default_rng(3)
    yy, xx = np.mgrid[0:size, 0:size].astype(float)
    background = 0.04 + 0.01 * np.sin(xx / 5.0)[..., None] * np.array([1.0, 0.8, 0.6]) + \
        0.004 * rng.random((size, size, 3))
    image = background.copy()
    c = size // 2
    image[c - 1:c + 1, c - 1:c + 1] += 30.0
    radius = np.full((size, size), radius_full)

    def far(full):
        half = 0.25 * (full[0::2, 0::2] + full[1::2, 0::2] + full[0::2, 1::2] + full[1::2, 1::2])
        g = size // 2
        mips = build_mips(make_layer(half, np.ones((g, g)), np.full((g, g), radius_full / 2)))
        gy, gx = np.mgrid[0:g, 0:g].astype(float)
        pre, cov, _ = gather(mips, gx + 0.5, gy + 0.5, np.full((g, g), radius_full / 2), rings,
                             near=False, shape=shape)
        return pre / np.maximum(cov, 1e-12)[..., None]

    g = size // 2
    gy, gx = np.mgrid[0:g, 0:g].astype(float)
    reference = far(background)
    energy = (image - background).sum(axis=(0, 1))
    truth = ideal_bokeh(gx, gy, float(c), float(c), radius_full, 1, shape, energy)
    if sprites:
        gather_input, kept, _ = extract_highlights(image, radius)
        light = far(gather_input) - reference
        for cell_energy, sx, sy, r in kept:
            # (DST_ALPHA, ONE) into the background, normalized by the
            # composite: exactly the sprite (test_sprite_dst_alpha_blend).
            light = light + live_sprite(gx, gy, sx, sy, abs(r), 1, shape, cell_energy)
    else:
        light = far(image) - reference
    interior = ideal_bokeh(gx, gy, float(c), float(c), 0.8 * radius_full, 1, shape,
                           np.ones(3))[..., 1] > 0.99 * ideal_bokeh(
        gx, gy, float(c), float(c), 0.8 * radius_full, 1, shape, np.ones(3))[..., 1].max()
    return light, truth, interior, energy


def evaluate_sprite_occluder(shape, occluder=True, size=128):
    """A light on a far backdrop (radius 20 full px) beside a nearer,
    sharper far surface (radius 8, also B2), through the far gather: the
    light's contribution over and outside the surface, for the gather alone,
    the sprite without and with the B2 visibility."""
    yy, xx = np.mgrid[0:size, 0:size].astype(float)
    background = np.full((size, size, 3), 0.04)
    background[..., 1] += 0.01 * np.sin(xx / 5.0)
    radius = np.full((size, size), 20.0)
    surface = (xx >= 72) & (xx < 100) if occluder else np.zeros((size, size), dtype=bool)
    background[surface] = (0.02, 0.05, 0.02)
    radius[surface] = 8.0
    image = background.copy()
    image[63:65, 63:65] += 30.0
    g = size // 2
    gy, gx = np.mgrid[0:g, 0:g].astype(float)

    def far(full):
        half = 0.25 * (full[0::2, 0::2] + full[1::2, 0::2] + full[0::2, 1::2] + full[1::2, 1::2])
        mips = build_mips(make_layer(half, np.ones((g, g)), radius[0::2, 0::2] / 2.0))
        kernel = far_kernel(mips, gx + 0.5, gy + 0.5)
        pre, cov, _ = gather(mips, gx + 0.5, gy + 0.5, kernel, 7, near=False, shape=shape)
        return pre / np.maximum(cov, 1e-12)[..., None], kernel

    reference, kernel = far(background)
    gathered = far(image)[0] - reference
    gather_input, kept, _ = extract_highlights(image, radius)
    naive = far(gather_input)[0] - reference
    visible = naive.copy()
    for energy, sx, sy, r in kept:
        sprite = live_sprite(gx, gy, sx, sy, abs(r), 1, shape, energy)
        naive += sprite
        visible += sprite * sprite_far_visibility(gx, gy, sx, sy, abs(r), shape,
                                                  kernel)[..., None]
    over = surface[0::2, 0::2]
    return {name: (float(v[over].sum()), float(v[~over].sum()))
            for name, v in (("gather", gathered), ("naive", naive), ("visible", visible))}


class LiveDoFTests(unittest.TestCase):
    def test_polygon_taps_partition_annuli(self):
        # Every ring must integrate its own annulus exactly, not merely
        # normalize the entire kernel (small sources reach only inner rings).
        for blades in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            for roundness in (0.0, 0.35, 1.0):
                for anamorphic in (0.1, 1.0, 2.0):
                    for rings in range(1, 8):
                        taps, s, area = aperture_taps(
                            rings, (blades, roundness, 0.37, anamorphic))
                        self.assertAlmostEqual(sum(t[3] for t in taps), area, places=11)
                        for k in range(1, rings + 1):
                            ring_area = sum(t[3] for t in taps if abs(t[2] - k * s) < 1e-12)
                            self.assertAlmostEqual(ring_area, area * 2.0 * k * s * s,
                                                   places=11)

    def test_polygon_uniform_coverage_for_any_kernel(self):
        # Integrate an infinite constant layer, including unclamped weights
        # and partial coverage. This catches both loss and gain of energy.
        for shape in ((0, 1.0, 0.0, 1.0), (3, 0.0, 0.2, 0.1),
                      (6, 0.0, 0.0, 1.0), (7, 0.35, 0.7, 2.0),
                      (12, 0.8, 1.2, 0.5)):
            for kernel in (0.5, 1.0, 1.5, 2.0, 2.5, 4.0, 8.0, 20.0):
                rings = int(np.clip(math.ceil(kernel - 0.5), 1, 7))
                taps, s, area = aperture_taps(rings, shape)
                for radius in (0.5, kernel * 0.75, kernel):
                    for alpha in (0.16, 1.0):
                        actual = sum(t[3] * kernel * kernel / area * alpha / (radius * radius) *
                                     reach_fraction(radius, t[2] * kernel, s * kernel, t[2] > 0)
                                     for t in taps)
                        self.assertAlmostEqual(float(actual), alpha, places=11)

    def test_polygon_tile_staircase(self):
        # Reproduces debug 9's tile-aligned coverage deficit. The old
        # circular-only model had no aperture-area approximation to expose.
        old_cov, old_image = evaluate_polygon_tiles(True)
        new_cov, new_image = evaluate_polygon_tiles(False)
        # Hexagon: six first-ring taps all sit at side midpoints, so their
        # estimated area is pi * 3/4 versus the true 3*sqrt(3)/2.
        expected = 1.0 / 9.0 + 8.0 / 9.0 * math.pi / (2.0 * math.sqrt(3.0))
        self.assertAlmostEqual(float(old_cov.min()), expected, places=10)
        self.assertGreater(float(np.max(np.abs(np.diff(old_cov, axis=1)))), 0.08)
        self.assertGreater(float(old_image.max()), 0.15)
        np.testing.assert_allclose(new_cov, 1.0, atol=1e-12)
        np.testing.assert_allclose(new_image, np.broadcast_to((0.12, 0.06, 0.03), new_image.shape),
                                   atol=1e-12)

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

    def test_self_occlusion_fills_behind_sharper_b1(self):
        # The jaw line: the light background showed through the neck below
        # an in-focus jaw whose edge is partly B1. The fill closes it and
        # leaves the neck's real see-through at the background edge as it
        # was. evaluate_far_strands() (strands over the far background) is
        # gated by test_strands_over_far_background with the fill on.
        before, edge_before = evaluate_self_occlusion(False)
        after, edge_after = evaluate_self_occlusion(True)
        self.assertGreater(before, 0.08)
        self.assertLess(after, 0.01)
        np.testing.assert_allclose(edge_after, edge_before, atol=1e-9)
        # A textured surface receding behind the focus (no occlusion):
        # coverage stays 1 and the colours as without the fill.
        yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(float)
        radius = 0.6 + 5.4 * xx / (SIZE - 1)
        color = np.zeros((SIZE, SIZE, 3))
        color[:] = (0.5, 0.5, 0.5)
        color[((xx // 2 + yy // 2) % 2) == 1] = (0.9, 0.7, 0.3)
        mips = build_mips(make_layer(color, np.ones((SIZE, SIZE)), radius))
        r_tiles, a_tiles = completed_radius(mips)
        kernel = tile_kernel(r_tiles, a_tiles)
        plain = gather(mips, xx + 0.5, yy + 0.5, kernel, 7, near=False)
        filled = gather(mips, xx + 0.5, yy + 0.5, kernel, 7, near=False, self_fill=True)
        sl = (slice(10, -10), slice(10, -10))
        np.testing.assert_allclose(filled[1][sl], 1.0, atol=1e-9)
        self.assertLess(float(np.abs(filled[2][sl] - plain[2][sl]).max()), 0.01)

    def test_split_surface_stays_opaque(self):
        # The user's grey veil: plain push-pull lets the far background
        # through a surface split between two bins.
        self.assertLess(evaluate_split_surface(None), 0.02)
        self.assertGreater(evaluate_split_surface(float("inf")), 0.1)

    def test_hidden_veil_fill_is_continuous(self):
        # The user's grey blocks: tile kernels must cover what the gathers
        # read, the completed bin.
        for rings in QUALITY_RINGS:
            self.assertGreater(evaluate_hidden_veil_tiles(rings, False), 0.5, rings)
            self.assertLess(evaluate_hidden_veil_tiles(rings, True), 0.02, rings)

    def test_minimum_radius_is_gathered(self):
        # Content just off the focus has the minimum radius, half a gather
        # pixel: its tile kernel is 0.5 up to 16-bit rounding. The gathers
        # skip only empty tiles (kernel 0) and read at least 0.5, so a
        # kernel a hair under 0.5 still gathers full coverage.
        h = w = SIZE
        yy, xx = np.mgrid[0:h, 0:w].astype(float)
        color = np.zeros((h, w, 3))
        color[:] = (0.7, 0.45, 0.3)
        weight = np.full((h, w), 0.16)
        mips = build_mips(make_layer(color, weight, np.full((h, w), 0.25)))
        kernel = np.full((h, w), 0.5 * (1.0 - 1e-3))
        self.assertTrue((kernel > 0.0).all())
        _, cov, _ = gather(mips, xx + 0.5, yy + 0.5, np.maximum(kernel, 0.5), 5, near=False,
                           self_fill=True)
        np.testing.assert_allclose(cov[8:-8, 8:-8], 0.16, atol=1e-3)

    def test_spherical_partitions_any_kernel(self):
        # The profile is integrated per tap exactly: a uniform field keeps
        # its coverage for any strength, shape, kernel and source radius.
        for shape in ((0, 1.0, 0.0, 1.0), (5, 0.0, 0.3, 1.0), (5, 0.5, 0.3, 1.33),
                      (6, 0.0, 0.0, 1.0), (6, 0.5, 0.0, 1.33)):
            for kernel in (0.5, 1.0, 2.5, 4.0, 8.0, 20.0):
                rings_list = sorted({int(np.clip(math.ceil(kernel - 0.5), 1, q))
                                     for q in QUALITY_RINGS})
                for rings in rings_list:
                    taps, s, area = aperture_taps(rings, shape)
                    for radius in (0.5, kernel * 0.6, kernel):
                        for sa in (-5.0, -2.5, -1.0, -0.4, 0.7, 1.0, 3.0, 5.0):
                            actual = sum(
                                t[3] * kernel * kernel / area / (radius * radius) *
                                reach_fraction(radius, t[2] * kernel, s * kernel, t[2] > 0, sa)
                                for t in taps)
                            self.assertAlmostEqual(float(actual), 1.0, places=11)

    def test_spherical_bokeh_profile(self):
        # An isolated light against a brute-force profiled splat: energy
        # exact; the azimuthal profile (the bokeh's look) within 1.5x of the
        # same gather's error without spherical aberration (an isolated
        # light's rim is softened by the tap spacing alone, 9% / 5% / 4% at
        # 3 / 5 / 7 rings), and its direction right: a > 0 brightens the
        # rim in front of the focus and the centre behind it.
        yy, xx = np.mgrid[0:SIZE, 0:SIZE]
        dist = np.hypot(xx - SIZE // 2, yy - SIZE // 2)
        bins = np.floor(dist / 2.0).astype(int)

        def radial(img):
            return np.array([img[bins == b].mean() for b in range(6)])

        for rings in QUALITY_RINGS:
            _, _, g0, t0, _ = evaluate("light", rings)
            r0 = radial(t0)
            base = np.sqrt(np.mean((radial(g0) - r0) ** 2)) / r0.max()
            for near in (True, False):
                # Beyond 1 the profile is cut and renormalized: a thin ring
                # (or a hard core), softened by the tap spacing at Low.
                for sa, limit in ((1.0, 1.5), (-1.0, 1.5), (0.5, 1.5), (3.0, 2.0), (-5.0, 2.0)):
                    _, _, g, t, energy = evaluate("light", rings, sa_strength=sa, near=near)
                    self.assertAlmostEqual(float(g.sum()), float(energy.sum()), delta=0.02)
                    rg, rt = radial(g), radial(t)
                    rms = np.sqrt(np.mean((rg - rt) ** 2)) / rt.max()
                    self.assertLess(rms, limit * base, (rings, near, sa))
                    rim_bright = (sa > 0) == near
                    self.assertEqual(rg[4] > rg[0], rim_bright, (rings, near, sa))

    def test_cat_eye_uniform_coverage_is_exact(self):
        # Taps and the open fraction integrate one barrel band: a uniform
        # field keeps coverage 1 for any barrel, shape, kernel, ring count,
        # source radius and spherical aberration (no tile steps).
        for shape in ((0, 1.0, 0.0, 1.0), (5, 0.0, 0.3, 1.0), (5, 0.5, 0.3, 1.33),
                      (6, 0.0, 0.0, 1.0), (6, 0.5, 0.0, 1.33)):
            for shift, angle in ((0.0, 0.0), (0.4, 0.3), (0.95, 2.0), (1.3, -1.0), (1.6, 0.5)):
                band = BarrelBand(np.array([math.cos(angle), math.sin(angle)]) * shift, shape)
                for kernel in (0.5, 1.0, 2.5, 6.0, 20.0):
                    for q in QUALITY_RINGS:
                        rings = int(np.clip(math.ceil(kernel - 0.5), 1, q))
                        taps, s, area = aperture_taps(rings, shape)
                        for radius in (0.3 * kernel, 0.6 * kernel, kernel):
                            for sa in (0.0, 1.0, -1.0, 3.0, -5.0):
                                actual = sum(
                                    t[3] * kernel * kernel / area / (radius * radius) *
                                    band.reach(radius, t[2] * kernel, s * kernel, t[2] > 0,
                                               sa, theta, half)
                                    for t, (theta, half) in zip(taps, tap_angles(rings)))
                                self.assertAlmostEqual(float(actual), 1.0, places=9)

    def test_cat_eye_open_fraction(self):
        # The band's open fraction against the exact vesica (circle).
        for shift in (0.3, 0.8, 1.2, 1.6):
            band = BarrelBand(np.array([shift, 0.0]), (0, 1.0, 0.0, 1.0))
            h = 0.5 * shift
            vesica = (2.0 * math.acos(h) - 2.0 * h * math.sqrt(1.0 - h * h)) / math.pi
            self.assertLess(abs(band.fraction[0] / vesica - 1.0), 0.03, shift)
        # No barrel shift on a circle: nothing clipped.
        self.assertAlmostEqual(BarrelBand(np.zeros(2), (0, 1.0, 0.0, 1.0)).fraction[0], 1.0,
                               places=12)

    def test_cat_eye_bokeh(self):
        # An isolated light against a brute-force clipped splat: energy
        # exact, error within the unclipped gather's, and the bokeh moved
        # toward the open side as the truth.
        yy, xx = np.mgrid[0:SIZE, 0:SIZE]
        for rings in QUALITY_RINGS:
            _, _, g0, t0, _ = evaluate("light", rings)
            base = np.sqrt(np.mean((g0 - t0) ** 2)) / t0.max()
            for near in (True, False):
                for shift, angle in ((0.8, 0.5), (1.2, 2.0), (1.6, 0.0)):
                    barrel = np.array([math.cos(angle), math.sin(angle)]) * shift
                    _, _, g, t, energy = evaluate("light", rings, near=near, barrel=barrel)
                    self.assertAlmostEqual(float(g.sum()), float(energy.sum()), delta=0.02)
                    rms = np.sqrt(np.mean((g - t) ** 2)) / t.max()
                    self.assertLess(rms, 1.05 * base, (rings, near, shift))
                    cg = np.array([(g * xx).sum(), (g * yy).sum()]) / g.sum() - SIZE // 2
                    cg0 = np.array([(g0 * xx).sum(), (g0 * yy).sum()]) / g0.sum() - SIZE // 2
                    ct = np.array([(t * xx).sum(), (t * yy).sum()]) / t.sum() - SIZE // 2
                    moved = cg - cg0
                    self.assertGreater(float(moved @ ct) / float(ct @ ct), 0.6,
                                       (rings, near, shift))

    def test_axial_ca_uniform_coverage_per_channel(self):
        # Each stratum's reach is exact and its energy rescaled: a uniform
        # field keeps coverage 1 in every channel for any shift, side,
        # shape, source radius, spherical aberration and barrel, once the
        # kernel spans the widest stratum (+ 0.75 delta).
        for shape in ((0, 1.0, 0.0, 1.0), (5, 0.0, 0.3, 1.33), (6, 0.0, 0.0, 1.0)):
            for barrel in (None, np.array([0.9, 0.4])):
                band = BarrelBand(barrel, shape) if barrel is not None else None
                for near in (True, False):
                    for delta in (0.3, 2.0, 6.0):
                        for radius in (0.5, 2.0, 8.0):
                            for sa_strength in (0.0, 1.0):
                                kernel = radius + 0.75 * delta
                                q = 7
                                rings = int(np.clip(math.ceil(kernel - 0.5), 1, q))
                                taps, s, area = aperture_taps(rings, shape)
                                total = np.zeros(3)
                                for t, (theta, half) in zip(taps, tap_angles(rings)):
                                    for offset, channels in ca_strata(delta, near):
                                        r_k = float(stratum_radius(radius, offset))
                                        sa = float(spherical_product(sa_strength, r_k, near))
                                        if band is None:
                                            reach = reach_fraction(r_k, t[2] * kernel, s * kernel,
                                                                   t[2] > 0, sa)
                                        else:
                                            reach = band.reach(r_k, t[2] * kernel, s * kernel,
                                                               t[2] > 0, sa, theta, half)
                                        total += (t[3] * kernel * kernel / area / (r_k * r_k) *
                                                  float(reach) * channels)
                                np.testing.assert_allclose(total, 1.0, atol=1e-9)

    def test_axial_ca_edge(self):
        # A hard blurred edge against a per-channel brute-force splat,
        # composited per channel: within 1% rms in the fringe band. One
        # (green) alpha instead loses a dark-over-bright edge's fringe: its
        # error is as large as the fringe itself, hence the per-channel
        # foreground veil (asDoFLiveGatherF.glsl, veil alpha).
        for rings in QUALITY_RINGS:
            for near in (True, False):
                for ratio in (0.2, 0.5, 1.0):
                    for front, back in (((1.0, 1.0, 1.0), (0.05, 0.05, 0.05)),
                                        ((0.05, 0.05, 0.05), (1.0, 1.0, 1.0)),
                                        ((0.9, 0.6, 0.4), (0.3, 0.5, 0.8))):
                        err, _, _ = evaluate_ca_edge(rings, ratio, front, back, near)
                        self.assertLess(err, 0.01, (rings, near, ratio, front))
        _, single, fringe = evaluate_ca_edge(7, 0.5, (0.05, 0.05, 0.05), (1.0, 1.0, 1.0))
        self.assertGreater(single, 0.8 * fringe)

    def test_field_curvature_capture_matches_library(self):
        # Field curvature (phase 4, step 1): the Mac OIT capture computes the
        # field position from gl_FragCoord with the uniforms asmacoit.cpp
        # uploads (xy = scale / size, zw = scale / 2); the Live library from
        # the pixel's uv. Both must give the same normalized CoC at every
        # pixel centre, or a fragment and the opaque surface at the same
        # depth would land in different bins.
        width, height = 3440, 1328
        aspect = width / height
        diagonal = math.hypot(aspect, 1.0)
        scale = np.array([2.0 * aspect / diagonal, 2.0 / diagonal])
        curvature = 0.37
        rng = np.random.default_rng(5)
        p = np.stack([rng.integers(0, width, 500), rng.integers(0, height, 500)], axis=-1)
        frag = p + 0.5
        field_capture = frag * (scale / np.array([width, height])) - 0.5 * scale
        uv = (p + 0.5) / np.array([width, height])
        field_library = (uv - 0.5) * scale
        np.testing.assert_allclose(field_capture, field_library, atol=1e-12)
        coc = rng.uniform(-1.2, 1.2, 500)
        both = [np.clip(coc + curvature * (f ** 2).sum(-1), -1.0, 1.0)
                for f in (field_capture, field_library)]
        np.testing.assert_allclose(both[0], both[1], atol=1e-12)
        # The frame corner is at field length 1.
        self.assertAlmostEqual(float(np.hypot(*(0.5 * scale))), 1.0, places=12)

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

    def test_sprite_light_is_flat(self):
        # Phase 3: a small light through the far gather leaves a ring pattern
        # inside its bokeh (6-22% relative std here); with its excess drawn
        # as a sprite the inside is flat and the energy is kept. The rms
        # against the point-light truth falls 2.5-8x; what remains is the
        # 2x2 px light's own edge softness, which the truth leaves out.
        for shape in ((3, 0.0, 0.0, 1.0), (6, 0.0, 0.0, 1.0), (0, 1.0, 0.0, 1.0),
                      (5, 0.5, 0.3, 1.0), (6, 0.0, 0.0, 1.33)):
            for rings in QUALITY_RINGS:
                light, truth, interior, energy = evaluate_sprite_light(rings, shape, True)
                base, _, _, _ = evaluate_sprite_light(rings, shape, False)
                g = light[..., 1]
                self.assertLess(g[interior].std() / g[interior].mean(), 0.01, (shape, rings))
                np.testing.assert_allclose(light.sum(axis=(0, 1)) * 4 / energy, 1.0, atol=0.01)
                rms = np.sqrt(((light - truth) ** 2).mean())
                rms_base = np.sqrt(((base - truth) ** 2).mean())
                self.assertLess(rms * 2.0, rms_base, (shape, rings))

    def test_sprite_matches_ideal_bokeh(self):
        # Every lens effect Live has, on the sprite alone: the energy is kept
        # within 3% (open fraction >= 0.25, radius >= 8 full px), 6% for the
        # smallest sprites (radius 3-5; none below 2, where the extraction
        # gate smoothstep(2, 4) is 0; worst: a 3 px triangle with strong CA,
        # whose smallest stratum is clamped to 1 px) and 12% for slivers the
        # barrel clips below a quarter of the aperture (radius 8 up). Mode
        # 1's vesica normalization was off by up to 58% for a clipped
        # triangle, and its shared barrel edge by up to 2.7x with strong CA.
        n = 64
        gy, gx = np.mgrid[0:n, 0:n].astype(float)
        energy = np.array([100.0, 100.0, 100.0])
        lenses = ({}, dict(barrel=(0.56, 0.56)), dict(barrel=(1.0, 0.99)), dict(delta=4.0),
                  dict(sa_strength=1.0), dict(sa_strength=-3.0), dict(sa_strength=5.0),
                  dict(barrel=(0.6, 0.5), delta=4.0, sa_strength=1.0),
                  dict(barrel=(-1.1, 0.4), delta=2.0, sa_strength=-3.0))
        for shape in ((3, 0.0, 0.0, 1.0), (6, 0.0, 0.0, 1.0), (0, 1.0, 0.0, 1.0),
                      (5, 0.5, 0.3, 1.0), (6, 0.0, 0.0, 1.33), (4, 0.2, 1.0, 0.7)):
            for lens in lenses:
                fraction = sprite_open_fraction(shape, lens.get("barrel"), 0.0)
                for plane in (1, -1):
                    for radius in (24.0, 8.0, 5.0, 3.0):
                        if fraction < 0.25 and radius < 8.0:
                            # The open part is under a few gather pixels
                            # (0.6 px at radius 3): no raster holds it.
                            continue
                        for offset in (0.3, 0.77):
                            sprite = live_sprite(gx, gy, n + offset, n + offset, radius, plane,
                                                 shape, energy, **lens)
                            error = np.abs(sprite.sum(axis=(0, 1)) * 4 / energy - 1.0).max()
                            limit = 0.12 if fraction < 0.25 else (0.03 if radius >= 8 else 0.06)
                            self.assertLess(error, limit, (shape, lens, plane, radius, offset))
        # The open fraction against a brute-force area, triangle at 1.4.
        grid = np.linspace(-1.2, 1.2, 1201)
        x, y = np.meshgrid(grid, grid)
        b, _ = sprite_boundary(np.arctan2(y, x), 3, 0.0)
        aperture = np.hypot(x, y) <= b
        brute = (aperture & (np.hypot(x - 1.0, y - 0.99) <= 1.0)).sum() / aperture.sum()
        self.assertAlmostEqual(sprite_open_fraction((3, 0.0, 0.0, 1.0), (1.0, 0.99), 0.0),
                               brute, delta=0.005)
        # Shape: a large sprite against the truth, rms within 3% of its peak.
        for shape in ((3, 0.0, 0.0, 1.0), (6, 0.0, 0.0, 1.33)):
            for lens in ({}, dict(barrel=(0.6, 0.5), delta=4.0, sa_strength=1.0)):
                sprite = live_sprite(gx, gy, n + 0.3, n + 0.3, 24.0, 1, shape, energy, **lens)
                truth = ideal_bokeh(gx, gy, n + 0.3, n + 0.3, 24.0, 1, shape, energy, **lens)
                self.assertLess(np.sqrt(((sprite - truth) ** 2).mean()) / truth.max(), 0.03)

    def test_sprite_energy_partition(self):
        # What the gather input loses is exactly what the kept cells carry:
        # every light keeps its energy, whatever the budget drops.
        rng = np.random.default_rng(11)
        size = 64
        image = 0.05 + 0.02 * rng.random((size, size, 3))
        for _ in range(12):
            y, x = rng.integers(4, size - 4, 2)
            image[y, x] += rng.uniform(2.0, 40.0)
        radius = np.where(np.arange(size)[None, :] < size // 2, 16.0, -9.0) * np.ones((size, 1))
        counts = []
        for budget in (4096, 3):
            gather_input, kept, excess = extract_highlights(image, radius, budget=budget)
            moved = sum((k[0] for k in kept), np.zeros(3))
            np.testing.assert_allclose(image.sum(axis=(0, 1)) - gather_input.sum(axis=(0, 1)),
                                       moved, rtol=1e-12)
            counts.append(len(kept))
        # The budget drops cells; their light stays in the gather input.
        self.assertGreater(counts[0], counts[1])

    def test_sprite_dst_alpha_blend(self):
        # Far sprites are blended (DST_ALPHA, ONE) into the premultiplied
        # background, whose alpha stays: the composite's normalization then
        # returns background + sprite exactly, at any coverage. Where the
        # background holds nothing (alpha 0: F covers the pixel) the light
        # adds nothing; it is hidden behind the focus.
        rng = np.random.default_rng(2)
        alpha = rng.uniform(0.05, 1.0, 100)
        color = rng.random((100, 3))
        sprite = rng.random((100, 3)) * 5.0
        stored = color * alpha[:, None] + sprite * alpha[:, None]
        np.testing.assert_allclose(stored / alpha[:, None], color + sprite, rtol=1e-12)
        # Alpha 0: the blend adds sprite * 0.
        np.testing.assert_array_equal(np.zeros(3) + sprite[0] * 0.0, np.zeros(3))

    def test_sprite_layer_scale(self):
        # Behind a strand in a nearer bin (V = T), the layer read alone holds
        # the full light and the veil attenuates it once in the composite:
        # scale 1. Behind glass in its own bin (V = 1): T. A surface split
        # 0.4 / 0.6 between B1 and B2 (V_B2 = 1 - 0.4 T): its layers
        # composite back to the whole light, with or without the strand.
        self.assertAlmostEqual(sprite_layer_scale(0.0, 1.0, 0.3, 1.0, 0.3), 1.0)
        self.assertAlmostEqual(sprite_layer_scale(0.0, 1.0, 0.3, 1.0, 1.0), 0.3)
        self.assertAlmostEqual(sprite_layer_scale(0.0, 1.0, 1.0, 1.0, 1.0), 1.0)
        for t in (1.0, 0.3):
            self.assertAlmostEqual(sprite_layer_scale(0.4, 0.6, t, t, t - 0.4 * t), 1.0)
        # Summing the shares instead would give 1.4 there.
        self.assertGreater(0.4 * 1.0 / 1.0 + 0.6 * 1.0 / 0.6, 1.3)

    def test_sprite_far_occlusion(self):
        # Palm leaves over a lit backdrop, both B2: the sprite without the
        # B2 visibility showed the light over the nearer leaves (14-23 of 90
        # units here), the gather alone almost nothing (0.1-0.3). With the
        # visibility at least 90% of that leak goes; without an occluder it
        # keeps the energy within 0.5%.
        for shape in ((3, 0.0, 0.0, 1.0), (6, 0.0, 0.0, 1.0), (0, 1.0, 0.0, 1.0)):
            result = evaluate_sprite_occluder(shape)
            self.assertLess(result["visible"][0], 0.1 * result["naive"][0], shape)
            self.assertAlmostEqual(result["visible"][1], result["naive"][1],
                                   delta=0.01 * result["naive"][1])
            clear = evaluate_sprite_occluder(shape, occluder=False)
            self.assertAlmostEqual(sum(clear["visible"]), sum(clear["naive"]),
                                   delta=0.005 * sum(clear["naive"]))

    def test_sprite_layer_order(self):
        # Far: the back part (B2) is drawn into B2 before B1 is laid over
        # it, so a B1 strand covers it like gathered light; the front part
        # (B1) is drawn over the pair. Near: the back part (N1) is weighted
        # by 1 - N2 coverage, the front part (N2) is added on top. Drawn
        # over the combined background instead, a B2 light showed over
        # the hair strands just behind the focus.
        rng = np.random.default_rng(4)
        strand = rng.uniform(0.0, 1.0, 50)           # B1 (or N2) coverage
        b1 = rng.random((50, 3)) * strand[:, None]   # premultiplied
        b2_alpha = rng.uniform(0.2, 1.0, 50)
        b2 = rng.random((50, 3)) * b2_alpha[:, None]
        light_back = rng.random((50, 3)) * 4.0
        light_front = rng.random((50, 3)) * 4.0
        # (DST_ALPHA, ONE) into B2, then the B1 pass: B1 + B2 / a2 (1 - a1).
        b2_lit = b2 + light_back * b2_alpha[:, None]
        background = b1 + b2_lit / b2_alpha[:, None] * (1.0 - strand[:, None])
        background += light_front                    # (DST_ALPHA, ONE), alpha 1
        expected = (b1 + light_front +
                    (b2 / b2_alpha[:, None] + light_back) * (1.0 - strand[:, None]))
        np.testing.assert_allclose(background, expected, rtol=1e-12)
        # The parts recompose the light of its layer pair.
        a_front, a_back = sprite_layer_parts(0.4, 0.6, 0.5, 0.5, 0.5 - 0.4 * 0.5)
        self.assertAlmostEqual(a_front + (1.0 - a_front) * a_back,
                               sprite_layer_scale(0.4, 0.6, 0.5, 0.5, 0.5 - 0.4 * 0.5))
        # The cell keeps their sum and the front fraction; both come back.
        total = a_front + a_back
        fraction = a_front / total
        self.assertAlmostEqual(total * fraction, a_front)
        self.assertAlmostEqual(total * (1.0 - fraction), a_back)

    def test_sprite_bright_highlights(self):
        # The Aperture-sampled renderer's artistic bright highlights on
        # Live's sprites: the same gain as mode 2 for an isolated light (its
        # ring test passes: the extraction found the light isolated), on the
        # sprites' energy only. The gather input is unchanged, so sharp and
        # large bright areas keep their light; strength 0 is off.
        from dof_reference import viewer_highlight_gain
        for strength in (0.0, 0.3, 1.0):
            for threshold in (0.5, 1.0, 4.0):
                for luminance in (0.2, 1.0, 3.0, 30.0):
                    for radius in (1.0, 3.0, 20.0, 200.0):
                        self.assertAlmostEqual(
                            live_highlight_gain(strength, threshold, luminance, radius),
                            viewer_highlight_gain(strength, threshold, luminance, 1e-6, radius))
        size = 64
        image = np.full((size, size, 3), 0.05)
        image[30:32, 30:32] = 20.0
        radius = np.full((size, size), 20.0)
        plain_input, plain, _ = extract_highlights(image, radius)
        boost_input, boosted, _ = extract_highlights(image, radius, boost=(0.3, 1.0))
        np.testing.assert_array_equal(plain_input, boost_input)
        gain = live_highlight_gain(0.3, 1.0, 20.0, 20.0)
        np.testing.assert_allclose(sum(k[0] for k in boosted), gain * sum(k[0] for k in plain),
                                   rtol=1e-12)
        _, off, _ = extract_highlights(image, radius, boost=(0.0, 1.0))
        np.testing.assert_allclose(sum(k[0] for k in off), sum(k[0] for k in plain), rtol=1e-12)

    def test_highlight_extraction_scope(self):
        # Only small isolated lights move: a bright area wider than the ring
        # and an in-focus light (blur under 2 px) stay in the gather.
        size = 64
        image = np.full((size, size, 3), 0.05)
        image[20:44, 20:44] = 8.0
        image[5, 5] = 30.0
        radius = np.full((size, size), 16.0)
        radius[0:12, 0:12] = 1.5
        _, kept, excess = extract_highlights(image, radius)
        self.assertEqual(len(kept), 0)
        self.assertEqual(float(excess.sum()), 0.0)

    def test_tap_table_matches_taps(self):
        # Phase 5, step 3: the gather reads its tap geometry from a per-frame
        # table (asdoflive.cpp) instead of computing it per tap. One 7-ring
        # table, uploaded as float32, must rebuild every ring count's taps,
        # for every shape, rotation and squeeze; the sector areas must still
        # partition each annulus (sum over a ring = its annulus area).
        for blades in (0, 5, 6, 3, 12):
            for roundness in (0.0, 0.5):
                for anamorphic in (1.0, 1.33, 0.1, 2.0):
                    for rotation in (0.0, math.radians(15.0), math.radians(-200.0)):
                        shape = (blades, roundness, rotation, anamorphic)
                        table = live_tap_table(shape)
                        self.assertEqual(len(table), 168)
                        for rings in QUALITY_RINGS:
                            expected, s, unit_area = aperture_taps(rings, shape)
                            rebuilt = taps_from_table(table, rings, anamorphic, unit_area)
                            np.testing.assert_allclose(np.array(rebuilt), np.array(expected),
                                                       rtol=1e-6, atol=1e-7,
                                                       err_msg=str((shape, rings)))
                            index = 1
                            for k in range(1, rings + 1):
                                ring = sum(t[3] for t in rebuilt[index:index + 6 * k])
                                annulus = unit_area * ((k + 0.5) ** 2 - (k - 0.5) ** 2) * s * s
                                self.assertAlmostEqual(ring / annulus, 1.0, delta=1e-6)
                                index += 6 * k


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
    print(f"split surface deficit: prior {COMPLETE_PRIOR} "
          f"{evaluate_split_surface(None):.4f}, plain push-pull "
          f"{evaluate_split_surface(float('inf')):.4f}")
    print("hidden veil coverage step: completed tiles " +
          ", ".join(f"{evaluate_hidden_veil_tiles(r, True):.4f}" for r in QUALITY_RINGS) +
          "; raw tiles " +
          ", ".join(f"{evaluate_hidden_veil_tiles(r, False):.4f}" for r in QUALITY_RINGS))
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
