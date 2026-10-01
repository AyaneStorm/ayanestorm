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
# Weight of the coarser estimate against the level's own visible density
# (asDoFLiveCommonF.glsl, LIVE_COMPLETE_PRIOR). None: plain push-pull.
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
    grey veil in squares). Read front to back:
        value += t S (1 + k) / (V + k),  t *= k (1 - V) / (V + k).
    Returns (values, found); found is false only where no level shows the
    bin."""
    k = COMPLETE_PRIOR if prior is None else prior
    top = len(mips) - 1
    out = np.zeros(x.shape + (6,))
    hidden = np.ones(x.shape)
    for step in range(top + 1):
        level = np.minimum(lod + step, top)
        v = sample_trilinear(mips, x, y, level)
        vis = v[..., 6]
        last = level >= top
        active = hidden > COMPLETE_EPSILON
        if k == float("inf"):
            gain = np.ones(x.shape)
            carry = 1.0 - vis
        else:
            gain = (1.0 + k) / (vis + k)
            carry = k * (1.0 - vis) / (vis + k)
        # Top level: normalize what is left by its visibility.
        share = np.where(last, hidden / np.maximum(vis, 1e-6) * (vis > 1e-6), hidden * gain)
        out += np.where(active[..., None], v[..., 0:6] * share[..., None], 0.0)
        hidden = np.where(active & ~last, hidden * carry, 0.0)
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


def spherical_reach(r, inner2, outer2, area, sa):
    r2 = np.maximum(r * r, 1e-12)
    hi = np.minimum(r2, outer2)
    lo = np.minimum(r2, inner2)
    g = lambda u: (1.0 + sa) * u - sa * u * u / r2
    return np.clip((g(hi) - g(lo)) / area, 0.0, None)


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
# Barrel shift cap, aperture radii (barrelCenter(), asDepthOfFieldFarF.glsl).
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
        ua = np.minimum(lo / r2, 1.0)
        ub = np.minimum(hi / r2, 1.0)
        a0 = area_to(theta - half, self.shape)
        a1 = area_to(theta + half, self.shape)
        m0, m1 = self.integral(a0, a1, theta - half, theta + half, ua, ub)
        f = np.maximum(self.fraction[0] + sa * self.fraction[1], 0.01)
        return np.maximum(m0 + sa * m1, 0.0) / ((a1 - a0) * (hi - lo) / r2) / f


def gather(mips, x, y, kernel_radius, rings, near, complete=True,
           shape=(0, 1.0, 0.0, 1.0), midpoint_areas=False, sa_strength=0.0,
           barrel=None):
    """Area-tap scatter-as-gather at pixels (x, y) (pixel centres).

    Returns (premultiplied rgb, coverage). kernel_radius: array per pixel.
    near: True for one shared kernel (coverage is the energy sum), False for
    the far layer (colour normalized, coverage clamped energy sum)."""
    taps, s_unit, unit_area = aperture_taps(rings, shape, midpoint_areas)
    R = np.maximum(kernel_radius, 0.5)
    s = s_unit * R
    color = np.zeros(x.shape + (3,))
    weight = np.zeros(x.shape)
    band = BarrelBand(barrel, shape) if barrel is not None else None
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
        sa = spherical_product(sa_strength, r_tap, near)
        if band is None:
            reach = reach_fraction(r_tap, d, s, d_unit > 0, sa)
        else:
            reach = band.reach(r_tap, d, s, d_unit > 0, sa, theta, half)
        wt = area / unit_area * E * reach
        rgb = v[..., 0:3] / np.maximum(W, 1e-12)[..., None]
        color += rgb * wt[..., None]
        weight += wt
    coverage = np.clip(weight, 0.0, 1.0)
    rgb = color / np.maximum(weight, 1e-12)[..., None]
    return rgb * coverage[..., None], coverage, rgb


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
            profiled = k * (1.0 - sa * (2.0 * rho2 - 1.0))
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
    _, cov, _ = gather(mips, xx + 0.5, yy + 0.5, kernel, rings, near=False)
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
                             shape=(6, 0.0, 0.0, 1.0), midpoint_areas=midpoint_areas)
        coverage[selected] = cov
        premultiplied[selected] = pre
    image = premultiplied + (1.0 - coverage)[..., None] * np.array((0.6, 0.6, 0.6))
    strip = (slice(16, 20), slice(8, 60))
    return coverage[strip], image[strip]


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
        _, cov, _ = gather(mips, xx + 0.5, yy + 0.5, np.maximum(kernel, 0.5), 5, near=False)
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
                        for sa in (-1.0, -0.4, 0.7, 1.0):
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
                for sa in (1.0, -1.0, 0.5):
                    _, _, g, t, energy = evaluate("light", rings, sa_strength=sa, near=near)
                    self.assertAlmostEqual(float(g.sum()), float(energy.sum()), delta=0.02)
                    rg, rt = radial(g), radial(t)
                    rms = np.sqrt(np.mean((rg - rt) ** 2)) / rt.max()
                    self.assertLess(rms, 1.5 * base, (rings, near, sa))
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
                            for sa in (0.0, 1.0, -1.0):
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
