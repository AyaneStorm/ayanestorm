"""Mode-1 DoF postfilter simulation (asDepthOfFieldPostfilterF.glsl).

Author: chanayane@firestorm

Point-tap near gather (asDepthOfFieldNearF.glsl, uniform-radius taps, random
per-pixel phase) on synthetic foreground scenes, then the postfilter, both
against a dense-sampling reference. Blur resolution equals full resolution
here (pixel scale 1).

    python dof_postfilter_sim.py

Scenes:
    veil  thin strands (2 px every 7 px), radius 20, over nothing
    edge  solid foreground, radius 4 (color 1) left, radius 24 (color 0) right
    disc  one solid 6x6 foreground block, radius 20: the spread disc rim
"""
import math

import numpy as np

GOLD = 2.399963229728653
W, H = 120, 80


def support(r, d):
    t = np.clip((d - (r - 1.0)) / 2.0, 0.0, 1.0)
    return 1.0 - t * t * (3.0 - 2.0 * t)


def spread_share(r):
    t = np.clip((r - 0.5) / 1.5, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gather(is_fg, radius, color, R, N, rng, phase=None):
    """Premultiplied (color * coverage, coverage) and radius moments."""
    py, px = np.mgrid[0:H, 0:W].astype(float)
    if phase is None:
        phase = rng.random((H, W)) * 2 * math.pi
    cov = np.zeros((H, W)); csum = np.zeros((H, W)); wsum = np.zeros((H, W))
    m1 = np.zeros((H, W)); m2 = np.zeros((H, W)); area = 0.0
    for i in range(N):
        fi = i + 0.5
        rho = fi / N
        aw = 2 * rho
        area += aw
        ang = fi * GOLD + phase
        x = np.clip((px + 0.5 + np.cos(ang) * rho * R).astype(int), 0, W - 1)
        y = np.clip((py + 0.5 + np.sin(ang) * rho * R).astype(int), 0, H - 1)
        fg = is_fg[y, x]
        r = radius[y, x]
        s = support(r, rho * R) * spread_share(r) * fg
        w = s * aw / np.maximum(r * r, 1.0)
        cov += w; csum += w * color[y, x]; wsum += w
        m1 += w * r; m2 += w * r * r
    cov = np.minimum(cov * R * R / area, 1.0)
    col = np.where(wsum > 1e-9, csum / np.maximum(wsum, 1e-12), 0.0)
    mom1 = np.where(wsum > 1e-9, m1 / np.maximum(wsum, 1e-12), 0.0)
    mom2 = np.where(wsum > 1e-9, m2 / np.maximum(wsum, 1e-12), 0.0)
    return np.stack([col * cov, cov]), mom1, mom2


def reference(is_fg, radius, color, R):
    rng = np.random.default_rng(7)
    acc = np.zeros((2, H, W))
    for p in np.linspace(0, 2 * math.pi, 8, endpoint=False):
        layer, _, _ = gather(is_fg, radius, color, R, 1024, rng,
                             np.full((H, W), p))
        acc += layer
    return acc / 8


# Empty-pixel radius probe (asDepthOfFieldPostfilterF.glsl).
PROBE = ((-1, 0), (1, 0), (0, -1), (0, 1), (-3, 0), (3, 0), (0, -3), (0, 3))


def postfilter(layer, m1, m2, R, N, plane_kind=1):
    """Mirror of asDepthOfFieldPostfilterF.glsl (point sampling)."""
    out = layer.copy()
    a = layer[1]
    # Empty pixels take their direct neighbours' coverage-weighted radius.
    m1c = m1.copy(); m2c = m2.copy()
    empty = a <= 1e-4
    s = np.zeros((3, H, W))
    for dx, dy in PROBE:
        ys = np.clip(np.arange(H) + dy, 0, H - 1)
        xs = np.clip(np.arange(W) + dx, 0, W - 1)
        an = a[np.ix_(ys, xs)]
        s += an * np.stack([m1[np.ix_(ys, xs)], m2[np.ix_(ys, xs)], np.ones((H, W))])
    fill = s[2] > 1e-4
    m1c[empty] = np.where(fill, s[0] / np.maximum(s[2], 1e-9), 0.0)[empty]
    m2c[empty] = np.where(fill, s[1] / np.maximum(s[2], 1e-9), 0.0)[empty]
    if plane_kind == 0:
        spacing = m1c * math.sqrt(math.pi / N)
    else:
        spacing = np.sqrt(2 * math.pi * np.maximum(m1c, 0.0) * R / N)
    width = np.minimum(0.5 * spacing, m1c / 3.0)
    active = (m1c >= 2.0) & (width >= 0.75)
    sigma = np.maximum(1.0, 0.15 * m1c)
    acc = layer.copy(); wsum = np.ones((H, W))
    py, px = np.mgrid[0:H, 0:W].astype(float)
    for i in range(12):
        fi = i + 0.5
        d = math.sqrt(fi / 12)
        ang = fi * GOLD
        x = np.clip(np.floor(px + 0.5 + math.cos(ang) * d * width).astype(int), 0, W - 1)
        y = np.clip(np.floor(py + 0.5 + math.sin(ang) * d * width).astype(int), 0, H - 1)
        n = layer[:, y, x]
        nm1 = m1[y, x]; nm2 = m2[y, x]
        ne = n[1] <= 1e-4
        nm1 = np.where(ne, m1c, nm1); nm2 = np.where(ne, m2c, nm2)
        w = math.exp(-2 * d * d) * np.exp(-(nm1 - m1c) ** 2 / (2 * sigma ** 2))
        w *= np.exp(-(n[1] - a) ** 2 / 0.13)
        acc += n * w; wsum += w
    filt = acc / wsum
    out[:, active] = filt[:, active]
    return out


def scene(name):
    is_fg = np.zeros((H, W), bool); radius = np.zeros((H, W)); color = np.zeros((H, W))
    if name == 'veil':
        xs = np.arange(W)
        strand = (xs % 7) < 2
        is_fg[:, strand] = True; radius[:] = 20.0; color[:] = 1.0
        R = 24.0
    elif name == 'edge':
        is_fg[:] = True
        radius[:, :W // 2] = 4.0; radius[:, W // 2:] = 24.0
        color[:, :W // 2] = 1.0
        R = 24.0
    else:
        is_fg[37:43, 57:63] = True; radius[:] = 20.0; color[:] = 1.0
        R = 24.0
    return is_fg, radius, color, R


def rms(x):
    return float(math.sqrt(np.mean(x * x)))


def main():
    rng = np.random.default_rng(3)
    interior = (slice(24, 56), slice(28, 92))
    for name in ('veil', 'edge', 'disc'):
        is_fg, radius, color, R = scene(name)
        ref = reference(is_fg, radius, color, R)
        for N in (16, 32, 96):
            layer, m1, m2 = gather(is_fg, radius, color, R, N, rng)
            filt = postfilter(layer, m1, m2, R, N)
            e0 = layer - ref; e1 = filt - ref
            line = '%-5s N=%2d  noise rms cov %.4f -> %.4f  rgb %.4f -> %.4f' % (
                name, N, rms(e0[1][interior]), rms(e1[1][interior]),
                rms(e0[0][interior]), rms(e1[0][interior]))
            mean0 = layer[1][interior].mean(); mean1 = filt[1][interior].mean()
            line += '  mean cov %.4f -> %.4f' % (mean0, mean1)
            if name == 'edge':
                # Color leak: premultiplied color error in the 8 px either
                # side of the radius boundary.
                band = (slice(24, 56), slice(W // 2 - 8, W // 2 + 8))
                line += '  edge rgb err %.4f -> %.4f' % (rms(e0[0][band]), rms(e1[0][band]))
            if name == 'disc':
                # Error around the spread disc, rim included.
                rim_band = (slice(30, 50), slice(30, 90))
                line += '  disc cov err %.4f -> %.4f' % (rms(e0[1][rim_band]),
                                                         rms(e1[1][rim_band]))
            print(line)


if __name__ == '__main__':
    main()
