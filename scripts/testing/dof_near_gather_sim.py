"""Advanced DoF near gather: point taps vs area (mip) taps. numpy.

Mirrors asDepthOfFieldNearF.glsl coverage for a circular aperture:
uniform-radius golden-angle taps over max radius R, source at +disk,
support 1 - smoothstep(r - 1, r + 1, d), weight 2 rho / r^2, normalized by
R^2 / sum(aperture weights). Reference: 2048 point taps, 3 phases.

Usage: python dof_near_gather_sim.py [uniform|mixed|ramp|face|lockface] [R] [split]
split: pyramid only for sources with r >= split (shader: 8 pi R / N, at
least 1 px; 6.3 at R = 24, N = 96). Colour error is weighted by the
reference coverage (what is visible).
Results 2026-09-28 (96 taps, R = 24; coverage rms error / colour rms error):
  uniform (strands r 18)       point 0.032          area 0.004 (either split)
  ramp                         point 0.040          split 1: 0.007
  mixed (strands r 4 and 20)   point 0.049          split 1: 0.014, split 6.3: 0.042
  lockface (strands r 14 over  point 0.045 / 0.033  split 1: 0.021 / 0.044
    a face r 2, colours 0/1)                        split 6.3: 0.045 / 0.011
  lockface, colour weighted 1/r^2, split 1: colour 0.109.
Split 1 lets a strand's colour replace the near-focused face's in shared
texels (seen as dark sharp strands in the viewer); the split fixes that at
the cost of point-tap noise for strands blurred less than the split.
Rejected designs measured here first: one mean radius per texel (-33% on
1:5 mixed radii), point values of the reach at 7 or 11 distances with
interpolation (+6..16% on a single radius or on mixed radii).

Band-averaged reach:

Band edges E_0 = 0, E_1 = 1, E_k = 1 + (R - 1) ((k - 1) / 10)^1.5, k = 1..11
(11 bands over [0, R]). Per source with r >= 1 and weight w = R^2 / r^2,
band k stores the area average of w [d < r] over the band:
    A_k = w (clamp(r, a, b)^2 - a^2) / (b^2 - a^2),   [a, b] = band k.
Linear in the sources, so mip averaging stays exact for any radius mix, and
the total (area-integrated) coverage is exact; only the placement of each
source's edge inside its band (a few px) is approximated.
"""
import math
import sys
import numpy as np

GOLD = 2.399963229728653
size = 96
R = float(sys.argv[2]) if len(sys.argv) > 2 else 24.0
NB = 11
E = [0.0, 1.0] + [1 + (R - 1) * (k / 10.0) ** 1.5 for k in range(1, 11)]
assert len(E) == NB + 1

yy, xx = np.mgrid[0:size, 0:size]
fg = np.zeros((size, size), bool)
for k, width in ((0, 1.0), (7, 1.5), (15, 2.0), (22, 1.0)):
    fg |= np.abs((xx - yy * 0.35) - (30 + k)) < width / 2
scene = sys.argv[1] if len(sys.argv) > 1 else 'uniform'
if scene == 'uniform':
    radius = np.where(fg, 0.75 * R, 0.0)
elif scene == 'mixed':
    radius = np.where(fg, np.where((xx + yy) % 7 < 3, R / 6, 0.83 * R), 0.0)
elif scene == 'ramp':  # radius varying continuously along the strands
    radius = np.where(fg, 1 + (R - 1) * yy / size, 0.0)
elif scene == 'face':
    face = (xx > 60) & (yy > 30) & (yy < 70)
    radius = np.where(fg, 0.75 * R, np.where(face, 0.6, 0.0))
else:  # 'lockface': strands r=0.6R, colour 0, over a solid face r=2, colour 1
    face = (xx > 20) & (xx < 90)
    radius = np.where(fg, 0.6 * R, np.where(face, 2.0, 0.0))
colour = np.where(fg, 0.0, 1.0)
isfg = radius > 0

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)

def support(r, d):
    return 1 - smoothstep(r - 1, r + 1, d)

w_full = np.where(isfg, R * R / np.maximum(radius ** 2, 1.0), 0.0)
split = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
big = isfg & (radius >= split)
w_big = np.where(big, w_full, 0.0)
A0 = []
for k in range(NB):
    a, b = E[k], E[k + 1]
    rc = np.clip(radius, a, b)
    A0.append(w_big * (rc ** 2 - a ** 2) / (b ** 2 - a ** 2))

def build_mips(img):
    levels = [img]
    while min(levels[-1].shape) > 1:
        a = levels[-1]
        h, w = a.shape[0] // 2, a.shape[1] // 2
        levels.append(a[:2*h, :2*w].reshape(h, 2, w, 2).mean(axis=(1, 3)))
    return levels

AM = [build_mips(a) for a in A0]
CM = build_mips(np.where(big, colour, 0.0))
NM = build_mips(np.where(big, 1.0, 0.0))

def bilinear(level, x, y):
    h, w = level.shape
    fx = x * w / size - 0.5
    fy = y * h / size - 0.5
    x0 = int(math.floor(fx)); y0 = int(math.floor(fy))
    tx = fx - x0; ty = fy - y0
    def at(i, j):
        return level[min(max(j, 0), h - 1), min(max(i, 0), w - 1)]
    return ((at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) +
            (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty)

def trilinear(levels, x, y, lod):
    lod = min(max(lod, 0.0), len(levels) - 1)
    l0 = int(math.floor(lod)); t = lod - l0
    a = bilinear(levels[l0], x, y)
    if t > 0 and l0 + 1 < len(levels):
        a = a * (1 - t) + bilinear(levels[l0 + 1], x, y) * t
    return a

def band(d):
    if d < 1.0:
        return 0
    return min(1 + int(math.floor(10 * ((d - 1) / (R - 1)) ** (2 / 3))), NB - 1)

def coverage(px, py, count, phase, mode):
    num = 0.0; area = 0.0; csum = 0.0; wsum = 0.0
    for i in range(count):
        fi = i + 0.5
        rho = fi / count
        ang = fi * GOLD + phase
        aw = 2 * rho
        area += aw
        x = px + 0.5 + math.cos(ang) * rho * R
        y = py + 0.5 + math.sin(ang) * rho * R
        d = rho * R
        ix = min(max(int(x), 0), size - 1); iy = min(max(int(y), 0), size - 1)
        if mode == 'point':
            if isfg[iy, ix]:
                c = aw * w_full[iy, ix] * support(radius[iy, ix], d)
                num += c; csum += c * colour[iy, ix]; wsum += c
        else:
            if isfg[iy, ix] and not big[iy, ix]:
                c = aw * w_full[iy, ix] * support(radius[iy, ix], d)
                num += c; csum += c * colour[iy, ix]; wsum += c
            spacing = math.sqrt(2 * math.pi * max(d, 0.5) * R / count)
            lod = math.log2(max(spacing, 1.0))
            c = aw * trilinear(AM[band(d)], x, y, lod)
            if c > 0:
                n = trilinear(NM, x, y, lod)
                col = trilinear(CM, x, y, lod) / max(n, 1e-9)
                num += c; csum += c * col; wsum += c
    return min(num / area, 1.0), (csum / wsum if wsum > 0 else 0.0)

rng = np.random.default_rng(1)
pix = [(px, py) for py in range(40, 56) for px in range(30, 80)]
refs = [[coverage(px, py, 2048, p, 'point') for p in (0.3, 2.1, 4.4)] for px, py in pix]
refv = np.array([np.mean([r[0] for r in rr]) for rr in refs])
refc = np.array([np.mean([r[1] for r in rr]) for rr in refs])
print('%s R=%g reference mean coverage %.4f' % (scene, R, refv.mean()))
for count in (96,):
    for mode in ('point', 'mip'):
        res = [coverage(px, py, count, rng.random() * 2 * math.pi, mode) for px, py in pix]
        vals = np.array([r[0] for r in res]); cols = np.array([r[1] for r in res])
        err = vals - refv
        cerr = (cols - refc) * refv  # colour error weighted by coverage (visible)
        print('%3d %-5s mean %.4f (bias %+.1f%%)  rms err %.4f  colour rms %.4f' % (
            count, mode, vals.mean(), 100 * (vals.mean() / refv.mean() - 1),
            math.sqrt((err ** 2).mean()), math.sqrt((cerr ** 2).mean())))
