#!/usr/bin/env python3
"""Mac OIT reference model. Author: chanayane@firestorm.

Run with Python 3; no viewer build or third-party packages are required.

    python scripts/testing/macoit_reference.py            # unit tests
    python scripts/testing/macoit_reference.py simulate   # error survey

Mirrors the Mac OIT GPU pipeline (indra/newview/asmacoit.cpp and the
asMacOIT*F.glsl shaders) per pixel, with float32 rounding emulated wherever
the GPU stores or blends in float32: layer keys, MIN/ADD blending, the merge
pass factorization and the per-fragment moment reconstruction. The result is
compared with exact back-to-front source-over compositing of the same
fragments, which is what Exact OIT produces.
"""

from dataclasses import dataclass
import math
import random
import struct
import sys
import unittest


def f32(x):
    """Round to the nearest float32, as a GPU register or RGBA32F texel."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def float_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def bits_float(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


# ---------------------------------------------------------------------------
# Constants mirrored from asMacOITCaptureF.glsl / asMacOITMomentsF.glsl
# ---------------------------------------------------------------------------

KEY_BIAS = 0x00800000
DEPTH_SCALE = 4194303  # 22 bits
EMPTY = bits_float(0x7F7FFFFF)
MIN_LAYER_ALPHA = 1.0 / 255.0
OVERESTIMATION = 0.25
BIAS_VECTOR = (0.0, 0.375, 0.0, 0.375)
NEAR, FAR = 0.1, 128.0


def depth01(distance):
    """Log-distance depth, 0 at the near plane, 1 at the far plane."""
    return min(max(math.log2(distance / NEAR) / math.log2(FAR / NEAR), 0.0), 1.0)


def depth_bits(d01):
    return int(d01 * DEPTH_SCALE + 0.5)


def make_key(bits, alpha):
    alpha_bits = int(min(max(alpha, 0.0), 1.0) * 255.0 + 0.5)
    return bits_float(((bits << 8) | alpha_bits) + KEY_BIAS)


def key_depth(key):
    return (float_bits(key) - KEY_BIAS) >> 8


def key_transmittance(key):
    return 1.0 - ((float_bits(key) - KEY_BIAS) & 255) / 255.0


def optical_depth(alpha):
    return -math.log(max(1.0 - alpha, 1.0 / 65536.0))


def absorbance_fraction(z, b1, b2, l21, inv_d11, inv_d22):
    """asMacOITMomentsF.glsl, float32 step by step."""
    z = f32(z)
    c0, c1, c2 = 1.0, z, f32(z * z)
    c1 = f32(c1 - b1)
    c2 = f32(c2 - f32(b2 + f32(l21 * c1)))
    c1 = f32(c1 * inv_d11)
    c2 = f32(c2 * inv_d22)
    c1 = f32(c1 - f32(l21 * c2))
    c0 = f32(c0 - f32(f32(c1 * b1) + f32(c2 * b2)))
    try:
        inv_c2 = f32(1.0 / c2)
        p = f32(c1 * inv_c2)
        q = f32(c0 * inv_c2)
        r = f32(math.sqrt(max(f32(f32(0.25 * p) * p) - q, 0.0)))
        z1 = f32(-0.5 * p - r)
        z2 = f32(-0.5 * p + r)
        f0 = OVERESTIMATION
        f1 = 1.0 if z1 < z else 0.0
        f2 = 1.0 if z2 < z else 0.0
        f01 = f32((f1 - f0) / (z1 - z))
        f12 = f32((f2 - f1) / (z2 - z1))
        f012 = f32((f12 - f01) / (z2 - z))
        p2 = f012
        p1 = f32(f01 - f32(f012 * f32(z + z1)))
        p0 = f32(f32(f0 - f32(f01 * z)) + f32(f32(f012 * z) * z1))
        fraction = f32(p0 + f32(p1 * b1) + f32(p2 * b2))
    except (ZeroDivisionError, OverflowError, ValueError):
        return 0.0  # the shader maps a NaN to "nothing in front"
    if fraction != fraction:
        return 0.0
    return min(fraction, 1.0) if fraction > 0.0 else 0.0


@dataclass(frozen=True)
class Fragment:
    distance: float
    alpha: float
    color: tuple


# ---------------------------------------------------------------------------
# Moment warps. Each maps the pixel's log depth to [-1, 1] for accumulation.
# ---------------------------------------------------------------------------

def warp_global(d, near_d, far_d):
    return 2.0 * d - 1.0


def warp_local(d, near_d, far_d):
    span = max(far_d - near_d, 1.0e-7)
    return 2.0 * min(max((d - near_d) / span, 0.0), 1.0) - 1.0


WARPS = {"global": warp_global, "local": warp_local, "last": warp_local}


def macoit_pixel(fragments, background, exact_layers=4, warp="last",
                 moment_bias=5.0e-7):
    """The Mac OIT pipeline for one pixel.

    warp="last" is the shipped pipeline (moments in the last peel pass,
    anchored at layer K-2). "local" and "global" are the rejected
    alternatives kept for the survey; None disables the tail term.
    """
    if exact_layers < 2:
        raise ValueError("Mac OIT needs K >= 2: the tail anchor is layer K-2")
    frags = []
    for f in fragments:
        d = depth01(f.distance)
        frags.append((f, d, depth_bits(d)))

    # KEYS: key0 by MIN; b0 by ADD; farthest depth by MIN of -depth.
    b0 = 0.0
    keys = [EMPTY] * 4
    far_d = 0.0
    for f, d, bits in frags:
        if f.alpha <= 0.0:
            continue
        b0 = f32(b0 + f32(optical_depth(f.alpha)))
        far_d = max(far_d, d)
        if f.alpha >= MIN_LAYER_ALPHA:
            keys[0] = min(keys[0], make_key(bits, f.alpha))

    # PEEL k: nearest key strictly behind layer k-1 (by depth bits).
    for k in range(1, exact_layers):
        previous = key_depth(keys[k - 1])
        for f, d, bits in frags:
            if f.alpha >= MIN_LAYER_ALPHA and bits > previous:
                keys[k] = min(keys[k], make_key(bits, f.alpha))

    # Moments: over the whole pixel (global), or behind an anchor layer with a
    # per-pixel [anchor, farthest] warp. "local" anchors at key0 (first peel
    # pass); "last" anchors at key K-2, accumulated in the last peel pass,
    # which is the last point where a known layer precedes the tail.
    moments = None
    if warp is not None and keys[exact_layers - 1] < EMPTY:
        anchor = keys[exact_layers - 2] if warp == "last" else keys[0]
        near_d = key_depth(anchor) / DEPTH_SCALE
        mapping = WARPS[warp]
        m0 = 0.0
        m = [0.0, 0.0, 0.0, 0.0]
        for f, d, bits in frags:
            if f.alpha <= 0.0 or (warp != "global" and bits <= key_depth(anchor)):
                continue
            od = f32(optical_depth(f.alpha))
            z = f32(mapping(d, near_d, far_d))
            z2 = f32(z * z)
            powers = (z, z2, f32(z2 * z), f32(z2 * z2))
            m0 = f32(m0 + od)
            m = [f32(m[i] + f32(od * powers[i])) for i in range(4)]
        # Merge pass.
        b = [f32(f32(m[i] / m0) * (1.0 - moment_bias) + BIAS_VECTOR[i] * moment_bias)
             for i in range(4)]
        d11 = f32(b[1] - f32(b[0] * b[0]))
        inv_d11 = f32(1.0 / d11) if d11 else float("inf")
        l21_d11 = f32(b[2] - f32(b[0] * b[1]))
        l21 = f32(l21_d11 * inv_d11)
        d22 = f32(f32(b[3] - f32(b[1] * b[1])) - f32(l21_d11 * l21))
        inv_d22 = f32(1.0 / d22) if d22 else float("inf")
        factor = (b[0], b[1], l21, inv_d11, inv_d22)
        last_d = key_depth(keys[exact_layers - 1]) / DEPTH_SCALE
        f_last = absorbance_fraction(mapping(last_d, near_d, far_d), *factor)
        moments = (m0, factor, mapping, near_d, f_last)

    # COLOR.
    sum_w = 0.0
    sum_cw = [0.0, 0.0, 0.0]
    for f, d, bits in frags:
        if f.alpha <= 0.0:
            continue
        t = 1.0
        tail = True
        for k in range(exact_layers):
            if bits <= key_depth(keys[k]):
                tail = False
                break
            t *= key_transmittance(keys[k])
        if tail and moments is not None:
            m0, factor, mapping, near_d, f_last = moments
            fraction = absorbance_fraction(mapping(d, near_d, far_d), *factor)
            t *= math.exp(-m0 * max(fraction - f_last, 0.0))
        w = f.alpha * t
        sum_w += w
        sum_cw = [sum_cw[i] + f.color[i] * w for i in range(3)]

    # RESOLVE.
    transmittance = math.exp(-b0)
    if sum_w <= 0.0:
        return [background[i] * transmittance for i in range(3)]
    return [sum_cw[i] * (1.0 - transmittance) / sum_w + background[i] * transmittance
            for i in range(3)]


def exact_pixel(fragments, background):
    color = list(background)
    for f in sorted(fragments, key=lambda f: -f.distance):
        color = [f.color[i] * f.alpha + color[i] * (1.0 - f.alpha) for i in range(3)]
    return color


# ---------------------------------------------------------------------------
# Scenes
# ---------------------------------------------------------------------------

HAIR = (0.35, 0.20, 0.10)
GLASS = (0.60, 0.70, 0.80)
BACKGROUND = (0.20, 0.50, 0.20)


def hair_scene(rng, base, strands, front_panes=0, back_pane=False):
    frags = [Fragment(base + rng.uniform(0.0, 0.012 * base), rng.uniform(0.2, 0.95),
                      tuple(c * rng.uniform(0.3, 1.6) for c in HAIR))
             for _ in range(strands)]
    for i in range(front_panes):
        frags.append(Fragment(base * 0.7 + 0.005 * i, 0.3, GLASS))
    if back_pane:
        frags.append(Fragment(base * 3.0, 0.4, GLASS))
    return frags


def random_scene(rng):
    kind = rng.choice(["hair", "hair+pane", "hair+2panes", "hair+back pane", "sheer"])
    base = rng.uniform(0.5, 5.0)
    if kind == "sheer":
        return kind, [Fragment(base, 0.95, (0.9, 0.1, 0.1)),
                      Fragment(base + 0.0005, 0.95, (0.1, 0.1, 0.9)),
                      Fragment(base + 0.002, 0.6, (0.1, 0.9, 0.1))]
    strands = rng.randint(3, 24)
    return kind, hair_scene(rng, base, strands,
                            front_panes={"hair+pane": 1, "hair+2panes": 2}.get(kind, 0),
                            back_pane=kind == "hair+back pane")


def error(fragments, **options):
    a = macoit_pixel(fragments, BACKGROUND, **options)
    b = exact_pixel(fragments, BACKGROUND)
    return max(abs(a[i] - b[i]) for i in range(3))


def survey(count=1500, seed=7):
    rng = random.Random(seed)
    scenes = [random_scene(rng) for _ in range(count)]
    print("max per-channel error against exact sorted compositing, %d pixels" % count)
    for layers in (2, 3, 4):
        for warp in (None, "global", "local", "last"):
            errors = sorted(error(f, exact_layers=layers, warp=warp) for _, f in scenes)
            pick = lambda p: errors[min(int(p * len(errors)), len(errors) - 1)]
            print("K=%d tail=%-7s mean %.4f  p95 %.4f  p99 %.4f  max %.4f" % (
                layers, warp or "none", sum(errors) / len(errors),
                pick(0.95), pick(0.99), errors[-1]))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class KeyEncoding(unittest.TestCase):
    def test_float_order_is_depth_then_alpha(self):
        rng = random.Random(1)
        pairs = [(rng.randint(0, DEPTH_SCALE), rng.random()) for _ in range(20000)]
        pairs += [(0, 0.0), (DEPTH_SCALE, 1.0), (0, 1.0), (DEPTH_SCALE, 0.0)]
        for p, q in zip(pairs, pairs[1:]):
            by_key = make_key(*p) < make_key(*q)
            by_value = (p[0], int(p[1] * 255 + 0.5)) < (q[0], int(q[1] * 255 + 0.5))
            self.assertEqual(by_key, by_value)

    def test_keys_are_normal_finite_floats(self):
        self.assertGreaterEqual(float_bits(make_key(0, 0.0)), 0x00800000)
        self.assertEqual(float_bits(make_key(DEPTH_SCALE, 1.0)), 0x407FFFFF)

    def test_empty_slot_sorts_behind_every_depth(self):
        self.assertGreater(key_depth(EMPTY), DEPTH_SCALE + 4)  # + glow tolerance

    def test_round_trip(self):
        key = make_key(123456, 0.5)
        self.assertEqual(key_depth(key), 123456)
        self.assertAlmostEqual(key_transmittance(key), 1.0 - 128 / 255.0)

    def test_distinct_layers_a_tenth_of_a_millimetre_apart(self):
        for distance in (0.3, 2.0, 20.0):
            self.assertLess(depth_bits(depth01(distance)),
                            depth_bits(depth01(distance + 0.0001 * distance / 2.0)) + 1)
            self.assertLess(depth_bits(depth01(distance)),
                            depth_bits(depth01(distance + 0.0001)))


class Exactness(unittest.TestCase):
    def test_up_to_k_layers_match_sorted_blending(self):
        rng = random.Random(2)
        for _ in range(300):
            count = rng.randint(1, 4)
            frags = [Fragment(rng.uniform(0.5, 50.0), rng.uniform(0.05, 1.0),
                              (rng.random(), rng.random(), rng.random()))
                     for _ in range(count)]
            # 8-bit key alpha is the only approximation left.
            self.assertLess(error(frags, exact_layers=4), 0.01)

    def test_opaque_front_layer_hides_everything_behind(self):
        frags = [Fragment(1.0, 1.0, (1.0, 0.0, 0.0))] + \
                [Fragment(1.0 + 0.01 * i, 0.5, (0.0, 1.0, 0.0)) for i in range(1, 10)]
        self.assertLess(error(frags), 1.0e-3)


class TailQuality(unittest.TestCase):
    def test_last_peel_anchor_beats_alternatives_behind_panes(self):
        rng = random.Random(3)
        scenes = [hair_scene(rng, rng.uniform(0.5, 5.0), rng.randint(8, 24), front_panes=2)
                  for _ in range(200)]
        mean = lambda warp: sum(error(f, warp=warp) for f in scenes) / len(scenes)
        last = mean("last")
        self.assertLess(last, mean("local"))
        self.assertLess(last, mean("global"))
        self.assertLess(last, mean(None))

    def test_survey_bounds_at_default_k(self):
        rng = random.Random(7)
        errors = sorted(error(f) for _, f in (random_scene(rng) for _ in range(600)))
        self.assertLess(sum(errors) / len(errors), 0.006)
        self.assertLess(errors[int(0.95 * len(errors))], 0.025)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "simulate":
        survey()
    else:
        unittest.main()
