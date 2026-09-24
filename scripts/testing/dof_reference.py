#!/usr/bin/env python3
"""Independent thin-lens visibility reference. Author: chanayane@firestorm.

Run with Python 3; no viewer build or third-party packages are required.
Coordinates are metres, +Z forward, and image coordinates at unit distance.
This deliberately uses ray/plane intersection, not the viewer's CoC/gather.
The viewer_* helpers mirror indra/newview/asdofcamera.cpp in the viewer's GL
convention (-Z forward eye space, matrices indexed m[column][row]) and are
checked against the independent ray model. Textured materials and general
blend factors are subsequent acceptance work; these tests do not certify
viewer integration.
"""

from dataclasses import dataclass
import math
import unittest


@dataclass(frozen=True)
class Camera:
    focus: float
    focal_length: float = 0.050
    f_number: float = 2.0

    def __post_init__(self):
        if not (0 < self.focal_length < self.focus and self.f_number > 0):
            raise ValueError("Require focus > focal length > 0 and f-number > 0")

    @property
    def aperture_radius(self):
        return self.focal_length / (2 * self.f_number)

    def ray(self, image, lens):
        """Aim a lens-origin ray at the pinhole ray's focal-plane point."""
        origin = (lens[0], lens[1], 0.0)
        direction = (image[0] * self.focus - lens[0],
                     image[1] * self.focus - lens[1], self.focus)
        return origin, direction

    def off_axis_view_and_projection(self, lens, near, far,
                                      tan_half_fov_x, tan_half_fov_y):
        """Eye-translation (view) plus asymmetric-frustum (projection) pair
        for a camera translated to `lens` on a plane parallel to the image
        plane (Kooima-style off-axis/generalized-perspective projection).

        Two effects combine to keep the focal plane's image fixed while the
        eye moves to `lens`, matching the pinhole-ray convention `ray()`
        uses:

        1. View: translate world points by `-lens` in x/y (the eye moved to
           `lens`, so points appear shifted by `-lens` in the new eye
           frame; z, the forward axis, is unaffected since `lens` lies in
           the z == 0 plane).
        2. Projection: an asymmetric frustum whose near-plane bounds are
           shifted by `-lens * (near / self.focus)`. This is the amount
           needed so that a point on the focal plane (z == self.focus),
           after the step-1 view shift by `-lens`, re-centers back onto the
           same NDC position it had from lens (0, 0) -- because at
           z == focus the view-shifted x is `image_x*focus - lens_x`, and
           dividing the frustum's asymmetric center by z == focus must
           exactly cancel the residual `-lens_x` term. Solving
           `(sx*(x - lens_x) + a*z) / z == x_pinhole/z` at z == focus with
           `a` derived from a frustum shift `s` gives `s = lens * near / focus`
           (near/far and fov shape otherwise standard, matching
           `LLViewerCamera::calcProjection`'s symmetric case at lens == 0).

        This reference keeps the file's +Z-forward convention throughout
        (`apply()` expects points with positive forward distance in z, not
        raw OpenGL -Z eye space); `w == z` here. The viewer-side C++ port
        additionally folds in the eye-space z negation
        (`LLViewerCamera::calcProjection`'s `mMatrix[2][3] = -1` becomes +1
        under this file's sign convention) when translated to GL's actual
        -Z-forward eye space -- a fixed, mechanical sign flip re-verified
        independently once written in C++, not a change to any ratio
        checked by the tests below.

        Returns (view_translate, projection): apply view first, then
        projection, i.e. `apply(projection, view_translate(point))`.
        """
        shift_x = -lens[0] * (near / self.focus)
        shift_y = -lens[1] * (near / self.focus)
        right = near * tan_half_fov_x + shift_x
        left = -near * tan_half_fov_x + shift_x
        top = near * tan_half_fov_y + shift_y
        bottom = -near * tan_half_fov_y + shift_y

        a = -(right + left) / (right - left)
        b = -(top + bottom) / (top - bottom)
        c = (far + near) / (far - near)
        d = -(2.0 * far * near) / (far - near)
        sx = 2.0 * near / (right - left)
        sy = 2.0 * near / (top - bottom)

        def view_translate(point):
            return (point[0] - lens[0], point[1] - lens[1], point[2])

        # Row-major 4x4 for this file's +Z-forward convention (w == z);
        # multiply as column-vector-on-the-right: M @ (x, y, z, 1).
        projection = (
            (sx, 0.0, a, 0.0),
            (0.0, sy, b, 0.0),
            (0.0, 0.0, c, d),
            (0.0, 0.0, 1.0, 0.0),
        )
        return view_translate, projection

    @staticmethod
    def apply(matrix, point):
        """Apply a 4x4 row-major projection matrix, returning clip-space xyzw."""
        x, y, z = point
        return tuple(
            row[0] * x + row[1] * y + row[2] * z + row[3]
            for row in matrix
        )


@dataclass(frozen=True)
class Card:
    depth: float
    bounds: tuple
    color: tuple
    alpha: float
    additive: bool = False

    def intersect(self, origin, direction):
        """Return positive ray distance if the finite card is intersected."""
        t = (self.depth - origin[2]) / direction[2]
        x, y = (origin[i] + t * direction[i] for i in (0, 1))
        left, right, bottom, top = self.bounds
        return t if t > 0 and left <= x < right and bottom <= y < top else None


def trace(camera, image, lens, cards, background=(0.0, 0.0, 0.0)):
    """Resolve actual ray hits far-to-near before any aperture averaging."""
    origin, direction = camera.ray(image, lens)
    hits = []
    for index, card in enumerate(cards):
        distance = card.intersect(origin, direction)
        if distance is not None:
            hits.append((distance, index, card))
    color, coverage = background, 0.0
    # Stable submission-index tie order is explicit, not a depth epsilon.
    for _, _, card in sorted(hits, key=lambda h: (-h[0], h[1])):
        if card.additive:
            color = tuple(c + v for c, v in zip(color, card.color))
        else:
            color = tuple(card.alpha * v + (1 - card.alpha) * c
                          for c, v in zip(color, card.color))
            coverage = card.alpha + (1 - card.alpha) * coverage
    return (*color, coverage)


# Exact OIT blend factor codes (asExactOITCompositeF.glsl blend_factor()).
(ONE, ZERO, DEST_COLOR, SOURCE_COLOR, ONE_MINUS_DEST_COLOR,
 ONE_MINUS_SOURCE_COLOR, DEST_ALPHA, SOURCE_ALPHA, ONE_MINUS_DEST_ALPHA,
 ONE_MINUS_SOURCE_ALPHA) = range(10)
# (color src, color dst, alpha src, alpha dst); None marks a glow-only node.
STANDARD_BLEND = (SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA, ZERO, ONE_MINUS_SOURCE_ALPHA)
GLOW_ONLY = None


def _blend_factor(code, src, dst):
    table = {
        ONE: (1.,) * 4, ZERO: (0.,) * 4, DEST_COLOR: dst, SOURCE_COLOR: src,
        ONE_MINUS_DEST_COLOR: tuple(1 - c for c in dst),
        ONE_MINUS_SOURCE_COLOR: tuple(1 - c for c in src),
        DEST_ALPHA: (dst[3],) * 4, SOURCE_ALPHA: (src[3],) * 4,
        ONE_MINUS_DEST_ALPHA: (1 - dst[3],) * 4,
        ONE_MINUS_SOURCE_ALPHA: (1 - src[3],) * 4,
    }
    return table.get(code, (0.,) * 4)


def viewer_blend_node(color, blend, node_glow, dst, glow):
    """Mirror of asExactOITCompositeF.glsl blend_node(); returns (dst, glow)."""
    if blend is GLOW_ONLY:
        return dst, glow + node_glow
    sf, df, asf, adf = (_blend_factor(code, color, dst) for code in blend)
    rgb = tuple(color[c] * sf[c] + dst[c] * df[c] for c in range(3))
    alpha = color[3] * asf[3] + dst[3] * adf[3]
    return (*rgb, alpha), node_glow + glow * (1 - color[3])


@dataclass(frozen=True)
class Surface:
    """Finite parallelogram at any orientation: origin + u*u_axis + v*v_axis
    for (u, v) in extent. texture(u, v) returns straight RGBA; alpha 0 or 1
    textures model alpha masks."""
    origin: tuple
    u_axis: tuple
    v_axis: tuple
    extent: tuple
    texture: object
    blend: tuple = STANDARD_BLEND
    glow: float = 0.0

    def intersect(self, origin, direction):
        """Return (t, u, v) for a forward hit inside the extent, else None."""
        # Solve origin + t*d = O + u*U + v*V with Cramer's rule.
        cols = (direction, tuple(-c for c in self.u_axis), tuple(-c for c in self.v_axis))
        rhs = tuple(self.origin[i] - origin[i] for i in range(3))

        def det(a, b, c):
            return (a[0] * (b[1] * c[2] - b[2] * c[1]) - b[0] * (a[1] * c[2] - a[2] * c[1])
                    + c[0] * (a[1] * b[2] - a[2] * b[1]))
        d = det(*cols)
        if abs(d) < 1e-300:
            return None
        t = det(rhs, cols[1], cols[2]) / d
        u = det(cols[0], rhs, cols[2]) / d
        v = det(cols[0], cols[1], rhs) / d
        u0, u1, v0, v1 = self.extent
        return (t, u, v) if t > 0 and u0 <= u < u1 and v0 <= v < v1 else None


def card_surface(card):
    """Fronto-parallel Card as a Surface with the standard alpha blend."""
    left, right, bottom, top = card.bounds
    return Surface((0., 0., card.depth), (1., 0., 0.), (0., 1., 0.),
                   (left, right, bottom, top),
                   lambda u, v: (*card.color, card.alpha))


def trace_viewer(camera, image, lens, surfaces, background=(0., 0., 0., 1.)):
    """Per-ray Exact OIT resolve: far-to-near, submission index on ties
    (comes_first()), viewer blend equations. Returns (r, g, b, a, glow)."""
    origin, direction = camera.ray(image, lens)
    hits = []
    for index, surface in enumerate(surfaces):
        hit = surface.intersect(origin, direction)
        if hit is not None:
            hits.append((hit, index, surface))
    dst, glow = background, 0.0
    for (t, u, v), _, surface in sorted(hits, key=lambda h: (-h[0][0], h[1])):
        dst, glow = viewer_blend_node(surface.texture(u, v), surface.blend,
                                      surface.glow, dst, glow)
    return (*dst, glow)


def viewer_jitter_projection(m, jitter_px, width, height):
    """Mirror of ASDoFCamera::jitterProjection: shift the image by jitter_px
    output pixels at every depth (clip x += 2*jx/W * w, with w == -z_eye)."""
    out = [list(column) for column in m]
    out[2][0] -= 2 * jitter_px[0] / width
    out[2][1] -= 2 * jitter_px[1] / height
    return out


def integrate_pixel(camera, pixel_center, pixel_size, surfaces, count,
                    shape=(0, 1., 0., 1.), radius=None, background=(0., 0., 0., 1.)):
    """Joint lens + pixel-box integration with the viewer sample sequence.
    pixel_center/pixel_size are image units (unit-distance image plane)."""
    radius = camera.aperture_radius if radius is None else radius
    lenses = viewer_aperture_samples(count, *shape)
    jitters = viewer_pixel_jitter(count)
    values = []
    for (lx, ly), (jx, jy) in zip(lenses, jitters):
        image = (pixel_center[0] + jx * pixel_size[0], pixel_center[1] + jy * pixel_size[1])
        values.append(trace_viewer(camera, image, (radius * lx, radius * ly),
                                   surfaces, background))
    return tuple(math.fsum(v[c] for v in values) / count for c in range(5))


def viewer_projection(fov_y, aspect, near, far):
    """Mirror of LLViewerCamera::calcProjection, indexed m[column][row]."""
    f = 1.0 / math.tan(fov_y * 0.5)
    m = [[0.0] * 4 for _ in range(4)]
    m[0][0] = f / aspect
    m[1][1] = f
    m[2][2] = (far + near) / (near - far)
    m[3][2] = (2 * far * near) / (near - far)
    m[2][3] = -1.0
    return m


def viewer_apply(m, point):
    """Column-major matrix times column vector (x, y, z, 1)."""
    p = (*point, 1.0)
    return tuple(math.fsum(m[c][r] * p[c] for c in range(4)) for r in range(4))


def viewer_lens_eye(eye_point, offset):
    """Mirror of ASDoFCamera::lensModelview: eye moved by offset (right, up)."""
    return (eye_point[0] - offset[0], eye_point[1] - offset[1], eye_point[2])


def viewer_lens_projection(m, offset, focus):
    """Mirror of ASDoFCamera::lensProjection: shear x/y by -z so eye-space
    z == -focus keeps its pinhole NDC after viewer_lens_eye."""
    out = [list(column) for column in m]
    out[2][0] -= m[0][0] * offset[0] / focus
    out[2][1] -= m[1][1] * offset[1] / focus
    return out


def viewer_focal_length_mm(fov_y, default_fov_y, default_focal_length_mm):
    """Mirror of the zoom mapping in LLPipeline::renderDoF (35mm-style)."""
    sensor_height = 2 * default_focal_length_mm * math.tan(default_fov_y / 2)
    return sensor_height / (2 * math.tan(fov_y / 2))


def viewer_coc_radius_pixels(aperture_radius, focus, depth, fov_y, height_px):
    """Mirror of ASDoFCamera::cocRadiusPixels (aperture-rim image offset)."""
    return (aperture_radius * abs(1 / focus - 1 / depth) *
            height_px / (2 * math.tan(fov_y / 2)))


# R4 Kronecker sequence (Roberts' generalized golden ratio, root of
# x^5 = x + 1). Dimensions 0/1 drive the lens, 2/3 the pixel jitter, so
# the two are decorrelated but share one nested index.
R4_ROOT = 1.16730397826141868426
R4_ALPHAS = tuple(1 / R4_ROOT ** k for k in range(1, 5))


def r4_point(index):
    return tuple((0.5 + a * index) % 1.0 for a in R4_ALPHAS)


def viewer_pixel_jitter(count):
    """Mirror of ASDoFAperture::generate's pixel output: box-filter offsets
    in [-0.5, 0.5) output pixels."""
    return [(p[2] - 0.5, p[3] - 0.5) for p in map(r4_point, range(count))]


def aperture_boundary(angle, blades, roundness):
    """Unit-circumradius aperture edge radius at a polar angle (rotation
    excluded). Polygon cos(pi/n)/cos(local) blended to a circle by roundness."""
    if blades < 3 or roundness >= 1:
        return 1.0
    half = math.pi / blades
    local = (angle % (2 * half)) - half
    return (1 - roundness) * math.cos(half) / math.cos(local) + roundness


def _blade_cdf(x, blades, roundness):
    """Integral of boundary(x)^2 / 2 over [-pi/n, x] within one blade."""
    half = math.pi / blades
    a, b = (1 - roundness) * math.cos(half), roundness

    def antiderivative(t):
        return (a * a * math.tan(t) + 2 * a * b * math.log(1 / math.cos(t) + math.tan(t))
                + b * b * t) / 2
    return antiderivative(x) - antiderivative(-half)


def viewer_aperture_samples(count, blades=0, roundness=1.0, rotation=0.0,
                            anamorphic=1.0):
    """Mirror of ASDoFAperture::generate: equal-weight, nested (prefixes of
    the R4 sequence), deterministic unit-aperture lens positions. Angles
    follow the boundary(angle)^2 area CDF inside each blade, so polygon
    corners get neither more nor less density than blade centres (polar
    area element: the angle marginal is proportional to boundary^2)."""
    polygon = blades >= 3 and roundness < 1
    samples = []
    for i in range(count):
        u, v = r4_point(i)[:2]
        if polygon:
            half = math.pi / blades
            blade_f = u * blades
            blade = math.floor(blade_f)
            target = (blade_f - blade) * _blade_cdf(half, blades, roundness)
            lo, hi = -half, half
            for _ in range(60):
                mid = (lo + hi) / 2
                if _blade_cdf(mid, blades, roundness) < target:
                    lo = mid
                else:
                    hi = mid
            # Blade 0 is centred on angle pi/n, i.e. local 0 maps to pi/n.
            angle = (lo + hi) / 2 + half + blade * 2 * half
        else:
            angle = 2 * math.pi * u
        r = math.sqrt(v) * aperture_boundary(angle, blades, roundness)
        angle += rotation
        samples.append((anamorphic * r * math.cos(angle), r * math.sin(angle)))
    return samples


def viewer_spectral_coordinate(index):
    """Mirror of ASDoFAperture::spectralCoordinate: base-2 radical inverse
    rotated by 1/2, mapped to s in [-1, 1) (blue -1, green 0, red +1)."""
    bits = int('{:032b}'.format(index & 0xffffffff)[::-1], 2)
    u = (bits / 4294967296.0 + 0.5) % 1.0
    return 2 * u - 1


def viewer_spectral_weights(s):
    """Mirror of ASDoFAperture::spectralWeights (RGB)."""
    return (1 + s, 1.5 * (1 - s * s), 1 - s)


def viewer_axial_ca_inv_focus(focus, focal_length, alpha, s):
    """Mirror of the renderer's per-sample focus: 1/S' = 1/S - s*alpha/(2f)."""
    return 1 / focus - s * 0.5 * alpha / focal_length


def disk_samples(radius, rings=32, sectors=128):
    """Dense deterministic equal-area disk quadrature for the reference."""
    for ring in range(rings):
        r = radius * math.sqrt((ring + 0.5) / rings)
        for sector in range(sectors):
            angle = 2 * math.pi * (sector + 0.5) / sectors
            yield r * math.cos(angle), r * math.sin(angle)


def integrate(camera, image, cards, radius=None, rings=32, sectors=128):
    radius = camera.aperture_radius if radius is None else radius
    values = [trace(camera, image, lens, cards)
              for lens in disk_samples(radius, rings, sectors)]
    return tuple(math.fsum(v[c] for v in values) / len(values) for c in range(4))


class ThinLensReferenceTests(unittest.TestCase):
    def setUp(self):
        self.camera = Camera(2.0)
        self.full = (-100.0, 100.0, -100.0, 100.0)

    def test_focal_plane_registration(self):
        for focus in (0.2, 2.0, 100.0):
            camera = Camera(focus)
            for image in ((0., 0.), (.5, -.3), (-.8, .6)):
                for lens in disk_samples(camera.aperture_radius, 4, 16):
                    origin, direction = camera.ray(image, lens)
                    t = focus / direction[2]
                    for axis in (0, 1):
                        self.assertAlmostEqual(origin[axis] + t * direction[axis],
                                               image[axis] * focus, places=13)

    def test_off_axis_projection_matrix_matches_ray_model_at_focal_plane(self):
        # Cross-check the matrix-form off-axis projection (asdofcamera's
        # planned basis) against the independent ray-cast model above: a
        # world point ON THE FOCAL PLANE, reached by any lens ray aimed at
        # a given pinhole image coordinate, must project through the
        # view+projection pair to that same NDC coordinate regardless of
        # lens offset. This is the defining "keep focal plane registered"
        # contract; off-focus-plane points are expected to project
        # differently per lens position (that disagreement IS the blur),
        # so this check is deliberately restricted to z == focus.
        near, far = 0.1, 1000.0
        tan_x = tan_y = math.tan(math.radians(30.0))
        for focus in (0.5, 2.0, 20.0):
            camera = Camera(focus)
            for image in ((0., 0.), (.4, -.2), (-.6, .5)):
                for lens in disk_samples(camera.aperture_radius, 3, 12):
                    origin, direction = camera.ray(image, lens)
                    t = focus / direction[2]
                    point = tuple(origin[a] + t * direction[a] for a in range(3))
                    self.assertAlmostEqual(point[2], focus, places=12)
                    view, projection = camera.off_axis_view_and_projection(
                        lens, near, far, tan_x, tan_y)
                    cx, cy, cz, cw = camera.apply(projection, view(point))
                    self.assertGreater(cw, 0.0)
                    self.assertAlmostEqual(cx / cw, image[0] / tan_x, places=9)
                    self.assertAlmostEqual(cy / cw, image[1] / tan_y, places=9)

    def test_off_axis_projection_reduces_to_symmetric_at_zero_lens(self):
        near, far = 0.1, 1000.0
        tan_x, tan_y = math.tan(math.radians(35.0)), math.tan(math.radians(20.0))
        camera = Camera(2.0)
        view, matrix = camera.off_axis_view_and_projection(
            (0., 0.), near, far, tan_x, tan_y)
        self.assertEqual(view((1.0, -2.0, 3.0)), (1.0, -2.0, 3.0))
        # This file's convention has w == z (not GL's raw w == -z_eye), so
        # the depth row carries the opposite sign from the textbook GL form;
        # test_off_axis_projection_depth_mapping_matches_symmetric below
        # independently confirms this still yields near -> -1, far -> +1.
        expected = (
            (1.0 / tan_x, 0.0, 0.0, 0.0),
            (0.0, 1.0 / tan_y, 0.0, 0.0),
            (0.0, 0.0, (far + near) / (far - near), -(2 * far * near) / (far - near)),
            (0.0, 0.0, 1.0, 0.0),
        )
        for row_a, row_b in zip(matrix, expected):
            for a, b in zip(row_a, row_b):
                self.assertAlmostEqual(a, b, places=12)

    def test_off_axis_projection_depth_mapping_matches_symmetric(self):
        # Off-axis shifting must not disturb the near/far depth mapping:
        # a point at z == near must map to NDC z == -1, z == far to +1,
        # exactly as the viewer's existing symmetric calcProjection does.
        near, far = 0.1, 1000.0
        tan_x = tan_y = math.tan(math.radians(30.0))
        camera = Camera(2.0)
        for lens in ((0., 0.), (0.01, -0.005)):
            view, matrix = camera.off_axis_view_and_projection(
                lens, near, far, tan_x, tan_y)
            for z, expected_ndc in ((near, -1.0), (far, 1.0)):
                _, _, cz, cw = camera.apply(matrix, view((0.0, 0.0, z)))
                self.assertAlmostEqual(cz / cw, expected_ndc, places=9)

    def test_off_axis_projection_inverts_ray(self):
        # Independent projection identity for a translated parallel camera.
        for depth in (.2, 1., 2., 8.):
            for lens in disk_samples(.025, 4, 16):
                point = (.04, -.02, depth)
                image = tuple((point[a] - lens[a]) / depth +
                              lens[a] / self.camera.focus for a in (0, 1))
                origin, direction = self.camera.ray(image, lens)
                for axis in (0, 1):
                    hit = origin[axis] + depth / direction[2] * direction[axis]
                    self.assertAlmostEqual(hit, point[axis], places=13)

    def test_blur_radius_and_sign(self):
        # Image coordinate recorded for an on-axis point by a lens sample is
        # where the lens->point line crosses the focal plane, over focus.
        # Previously this compared the formula with itself.
        focus = self.camera.focus
        for lens_x in (.025, -.025):
            for depth in (.5, 1., 2., 4., 20.):
                crossing = lens_x + (focus / depth) * (0. - lens_x)
                image = (crossing / focus, 0.)
                origin, direction = self.camera.ray(image, (lens_x, 0.))
                t = depth / direction[2]
                self.assertAlmostEqual(origin[0] + t * direction[0], 0., places=13)
                self.assertAlmostEqual(abs(image[0]),
                                       abs(lens_x) * abs(1 / focus - 1 / depth), places=13)
                if depth != focus:
                    self.assertEqual(image[0] * lens_x > 0, depth > focus)
        self.assertAlmostEqual(Camera(2., f_number=4.).aperture_radius * 2,
                               self.camera.aperture_radius)

    def _viewer_cases(self):
        for fov_deg in (10., 60., 120.):
            for aspect in (.5, 16 / 9, 3.):
                for focus in (.3, 2., 200.):
                    yield math.radians(fov_deg), aspect, focus

    def test_viewer_lens_projection_focal_plane_pixel_error(self):
        # Gate: focal-plane reprojection error < 0.01 output pixel across
        # lens positions, FOV, aspect and resolution, in the viewer's GL
        # convention. Ray model supplies the world point (+Z forward); GL
        # eye space is (x, y, -z).
        near, far = .1, 1024.
        for fov_y, aspect, focus in self._viewer_cases():
            camera = Camera(focus, focal_length=min(.2, focus / 2), f_number=1.)
            tan_y = math.tan(fov_y / 2)
            tan_x = tan_y * aspect
            base = viewer_projection(fov_y, aspect, near, far)
            for image in ((0., 0.), (.7 * tan_x, -.6 * tan_y), (-.95 * tan_x, .9 * tan_y)):
                for lens in disk_samples(camera.aperture_radius, 2, 8):
                    origin, direction = camera.ray(image, lens)
                    t = focus / direction[2]
                    world = [origin[a] + t * direction[a] for a in range(3)]
                    eye = viewer_lens_eye((world[0], world[1], -world[2]), lens)
                    proj = viewer_lens_projection(base, lens, focus)
                    cx, cy, _, cw = viewer_apply(proj, eye)
                    for height in (720, 2160, 8640):
                        width = height * aspect
                        err_x = abs(cx / cw - image[0] / tan_x) * width / 2
                        err_y = abs(cy / cw - image[1] / tan_y) * height / 2
                        self.assertLess(max(err_x, err_y), .01)

    def test_viewer_lens_projection_preserves_depth_and_pinhole(self):
        near, far = .1, 1024.
        base = viewer_projection(math.radians(60.), 16 / 9, near, far)
        self.assertEqual(viewer_lens_projection(base, (0., 0.), 2.), base)
        proj = viewer_lens_projection(base, (.01, -.02), 2.)
        for z, expected in ((-near, -1.), (-far, 1.)):
            _, _, cz, cw = viewer_apply(proj, (.3, -.1, z))
            self.assertAlmostEqual(cz / cw, expected, places=9)

    def test_viewer_coc_matches_projected_disparity(self):
        # Pixel CoC formula equals the rim sample's projected displacement
        # of an off-focus point; far and near points move in opposite senses.
        near, far = .1, 1024.
        for fov_y, aspect, focus in self._viewer_cases():
            radius = .01
            base = viewer_projection(fov_y, aspect, near, far)
            proj = viewer_lens_projection(base, (0., radius), focus)
            for depth in (focus / 3, focus * 4):
                point = (0., 0., -depth)
                cx0, cy0, _, cw0 = viewer_apply(base, point)
                cx, cy, _, cw = viewer_apply(proj, viewer_lens_eye(point, (0., radius)))
                height = 2160
                shift = (cy / cw - cy0 / cw0) * height / 2
                expected = viewer_coc_radius_pixels(radius, focus, depth, fov_y, height)
                self.assertAlmostEqual(abs(shift), expected, delta=1e-6 * max(1., expected))
                # Eye moved up: near points drop, far points rise.
                self.assertEqual(shift > 0, depth > focus)

    APERTURE_SHAPES = ((0, 1., 0., 1.), (6, 0., 0., 1.), (5, .35, .4, 1.),
                       (3, 0., 1., 1.), (9, .7, 0., 1.5))

    @staticmethod
    def _inside(point, blades, roundness, rotation, anamorphic, slack=1e-9):
        x, y = point[0] / anamorphic, point[1]
        angle = math.atan2(y, x) - rotation
        return math.hypot(x, y) <= aperture_boundary(angle, blades, roundness) + slack

    def test_aperture_samples_inside_shape_nested_and_deterministic(self):
        for shape in self.APERTURE_SHAPES:
            samples = viewer_aperture_samples(512, *shape)
            self.assertEqual(samples, viewer_aperture_samples(512, *shape))
            self.assertEqual(samples[:128], viewer_aperture_samples(128, *shape))
            for point in samples:
                self.assertTrue(self._inside(point, *shape))

    def test_aperture_samples_uniform_area_density(self):
        # Fraction of samples in test regions must equal the region's share
        # of aperture area, measured independently on a dense grid. Regions:
        # half-planes (catches centroid bias) and the outer radial band
        # (catches corner/blade-centre density errors).
        n, grid = 4096, 400
        for shape in self.APERTURE_SHAPES:
            anamorphic = shape[3]
            samples = viewer_aperture_samples(n, *shape)
            cells = [((i + .5) / grid * 2 - 1) * anamorphic for i in range(grid)]
            rows = [(j + .5) / grid * 2 - 1 for j in range(grid)]
            area_points = [(x, y) for x in cells for y in rows
                           if self._inside((x, y), *shape, slack=0.)]
            regions = [
                lambda p: p[0] > 0,
                lambda p: p[1] > .2,
                lambda p: p[0] + p[1] < -.3,
                lambda p: math.hypot(p[0] / anamorphic, p[1]) >
                    .8 * aperture_boundary(math.atan2(p[1], p[0] / anamorphic) - shape[2],
                                           shape[0], shape[1]),
            ]
            for region in regions:
                expected = sum(map(region, area_points)) / len(area_points)
                actual = sum(map(region, samples)) / n
                self.assertLess(abs(actual - expected), .01, (shape, expected, actual))

    def test_aperture_samples_converge_on_analytic_strip(self):
        # Circular aperture sampler reproduces the analytic strip coverage
        # used by test_aperture_convergence (same geometry).
        card = Card(1., (-.003, .003, -1., 1.), (1., 1., 1.), .4)
        radius = .025
        q = .003 / ((1 - card.depth / self.camera.focus) * radius)
        expected = .4 * 2 / math.pi * (math.asin(q) + q * math.sqrt(1 - q * q))
        samples = viewer_aperture_samples(4096)
        value = math.fsum(trace(self.camera, (0., 0.), (radius * x, radius * y), [card])[3]
                          for x, y in samples) / len(samples)
        self.assertLess(abs(value - expected), .002)

    def test_axial_ca_weights_neutral_and_nested(self):
        # Sample 0 is green; every channel averages to 1 over nested
        # prefixes, so in-focus content stays neutral and brightness is
        # preserved; s stays in [-1, 1). Bound: Koksma-Hlawka, weight
        # variation <= 3 times van der Corput discrepancy <= (log2 N + 3)/(3N).
        self.assertEqual(viewer_spectral_coordinate(0), 0.0)
        for count in (16, 64, 256, 1000, 2048):
            s_values = [viewer_spectral_coordinate(i) for i in range(count)]
            self.assertTrue(all(-1 <= s < 1 for s in s_values))
            for channel in range(3):
                mean = math.fsum(viewer_spectral_weights(s)[channel] for s in s_values) / count
                self.assertLess(abs(mean - 1), (math.log2(count) + 3) / count, (count, channel))

    def test_axial_ca_focus_shift_orders_channels(self):
        # Red (s = +1) focuses farther than green, blue nearer; the red-blue
        # spread is alpha / f in inverse focus.
        focus, f, alpha = 2.0, 0.05, 0.002
        red = viewer_axial_ca_inv_focus(focus, f, alpha, 1.0)
        green = viewer_axial_ca_inv_focus(focus, f, alpha, 0.0)
        blue = viewer_axial_ca_inv_focus(focus, f, alpha, -1.0)
        self.assertLess(red, green)
        self.assertLess(green, blue)
        self.assertAlmostEqual(blue - red, alpha / f, places=12)
        self.assertAlmostEqual(green, 1 / focus, places=12)

    def test_viewer_blend_standard_matches_trace(self):
        red = Card(1., self.full, (1., 0., 0.), .5)
        blue = Card(4., self.full, (0., 0., 1.), .5)
        for cards in ([red, blue], [blue, red]):
            expected = trace(self.camera, (0., 0.), (0., 0.), cards)
            got = trace_viewer(self.camera, (0., 0.), (0., 0.),
                               [card_surface(c) for c in cards])
            for c in range(3):
                self.assertAlmostEqual(got[c], expected[c])
            # Viewer alpha under the standard tuple is transmittance.
            self.assertAlmostEqual(got[3], 1 - expected[3])

    def test_viewer_blend_nonstandard_is_order_dependent(self):
        multiply = (DEST_COLOR, ZERO, ZERO, ONE)

        def surface(depth, color, blend):
            return Surface((0., 0., depth), (1., 0., 0.), (0., 1., 0.), self.full,
                           lambda u, v: color, blend)
        grey = (.5, .5, .5, 1.)
        near_blue = [surface(4., (1., .5, 0., 1.), multiply),
                     surface(1., (0., 0., 1., .5), STANDARD_BLEND)]
        near_mult = [surface(1., (1., .5, 0., 1.), multiply),
                     surface(4., (0., 0., 1., .5), STANDARD_BLEND)]
        a = trace_viewer(self.camera, (0., 0.), (0., 0.), near_blue, grey)
        b = trace_viewer(self.camera, (0., 0.), (0., 0.), near_mult, grey)
        for got, want in ((a, (.25, .125, .5, .5)), (b, (.25, .125, 0., .5))):
            for g, w in zip(got, want):
                self.assertAlmostEqual(g, w)

    def test_viewer_glow_accumulation_order(self):
        def surface(depth, blend, glow, alpha=.5):
            return Surface((0., 0., depth), (1., 0., 0.), (0., 1., 0.), self.full,
                           lambda u, v: (1., 1., 1., alpha), blend, glow)
        far_glow = [surface(4., GLOW_ONLY, 2.), surface(1., STANDARD_BLEND, .1)]
        near_glow = [surface(1., GLOW_ONLY, 2.), surface(4., STANDARD_BLEND, .1)]
        self.assertAlmostEqual(trace_viewer(self.camera, (0., 0.), (0., 0.), far_glow)[4], 1.1)
        self.assertAlmostEqual(trace_viewer(self.camera, (0., 0.), (0., 0.), near_glow)[4], 2.1)

    def test_angled_textured_surface_sharp_only_on_focal_line(self):
        # Tilted, striped alpha-mask plane crossing the focal plane at x == 0.
        focus = self.camera.focus

        def stripes(u, v):
            band = math.floor(u * 1000 + .5) % 2
            return (u % 1., .3, .7, 1. if band else 0.)
        plane = Surface((0., 0., focus), (1., 0., .8), (0., 1., 0.),
                        (-10., 10., -10., 10.), stripes)
        back = card_surface(Card(8., self.full, (0., 1., 0.), 1.))
        scene = [plane, back]
        pinhole = trace_viewer(self.camera, (0., 0.), (0., 0.), scene)
        lens = [(self.camera.aperture_radius * x, self.camera.aperture_radius * y)
                for x, y in viewer_aperture_samples(64)]
        for sample in lens:
            got = trace_viewer(self.camera, (0., 0.), sample, scene)
            for g, w in zip(got, pinhole):
                self.assertAlmostEqual(g, w, places=12)
        # x == .5 hits the plane at z ~ 3.3 m: blur ~ 4 stripe periods.
        off = (.5, 0.)
        off_pinhole = trace_viewer(self.camera, off, (0., 0.), scene)
        blurred = [math.fsum(trace_viewer(self.camera, off, s, scene)[c] for s in lens) / len(lens)
                   for c in range(5)]
        self.assertGreater(max(abs(a - b) for a, b in zip(blurred, off_pinhole)), .05)

    def test_pixel_footprint_removes_subpixel_strand_aliasing(self):
        # Focused strand 0.3 px wide: centre-point sampling is 0 or alpha;
        # joint lens+pixel sampling gives alpha * 0.3 for every sub-pixel
        # position (no appear/disappear as the strand moves).
        focus, pixel, width, alpha = self.camera.focus, 1e-3, 3e-4, .6
        centre_values, footprint_values = set(), []
        for step in range(8):
            # Strand stays inside the pixel: offsets -.45 .. +.145 px.
            offset = (-.45 + step * .085) * pixel
            strand = card_surface(Card(focus, ((offset) * focus, (offset + width) * focus,
                                               -100., 100.), (1., 1., 1.), alpha))
            centre_values.add(round(trace_viewer(self.camera, (0., 0.), (0., 0.), [strand])[0], 9))
            footprint_values.append(integrate_pixel(self.camera, (0., 0.), (pixel, pixel),
                                                    [strand], 256)[0])
        self.assertEqual(centre_values, {0., alpha})
        for value in footprint_values:
            self.assertLess(abs(value - alpha * width / pixel), .01)

    def test_defocused_strand_energy_converges(self):
        # Isolated unoccluded strand gate: integrated contribution across the
        # blurred footprint (plus guard band) within 1% of alpha * width once
        # converged. Joint lens+pixel sampling is unbiased, but a defocused
        # sub-pixel strand is a thin discontinuous integrand: measured error
        # is ~10% at 256 and ~2.5% at 4096 samples (see plan record), so
        # the 1% gate is asserted at 16384. Low counts are not converged.
        pixel, width, alpha = 1e-3, 3e-4, .6
        strand = card_surface(Card(1., (0., width, -100., 100.), (1., 1., 1.), alpha))
        blur = self.camera.aperture_radius * abs(1 / self.camera.focus - 1.)
        span = int(blur / pixel) + 4
        total = math.fsum(
            integrate_pixel(self.camera, (k * pixel, 0.), (pixel, pixel), [strand], 16384)[0]
            for k in range(-span, span + 1)) * pixel
        self.assertLess(abs(total - alpha * width) / (alpha * width), .01)

    def test_viewer_jitter_projection_shifts_all_depths(self):
        width, height = 1920, 1080
        base = viewer_projection(math.radians(60.), width / height, .1, 1024.)
        jitter = (.37, -.21)
        moved = viewer_jitter_projection(base, jitter, width, height)
        for depth in (.2, 2., 500.):
            point = (.1, -.3, -depth)
            x0, y0, _, w0 = viewer_apply(base, point)
            x1, y1, _, w1 = viewer_apply(moved, point)
            self.assertAlmostEqual((x1 / w1 - x0 / w0) * width / 2, jitter[0], places=9)
            self.assertAlmostEqual((y1 / w1 - y0 / w0) * height / 2, jitter[1], places=9)
        # Lens shear and jitter touch disjoint terms' sums: order-independent.
        a = viewer_jitter_projection(viewer_lens_projection(base, (.01, .02), 2.), jitter, width, height)
        b = viewer_lens_projection(viewer_jitter_projection(base, jitter, width, height), (.01, .02), 2.)
        for ca, cb in zip(a, b):
            for x, y in zip(ca, cb):
                self.assertAlmostEqual(x, y, places=12)

    def test_viewer_focal_length_zoom_mapping(self):
        default_fov = math.radians(60.)
        self.assertAlmostEqual(viewer_focal_length_mm(default_fov, default_fov, 50.), 50.)
        zoomed = 2 * math.atan(math.tan(default_fov / 2) / 2)
        self.assertAlmostEqual(viewer_focal_length_mm(zoomed, default_fov, 50.), 100.)

    def test_pinhole_equivalence(self):
        cards = [Card(1., self.full, (.8, .1, .2), .4),
                 Card(4., self.full, (.2, .5, .9), .7)]
        direct = trace(self.camera, (0., 0.), (0., 0.), cards)
        averaged = integrate(self.camera, (0., 0.), cards, radius=0., rings=2, sectors=8)
        for a, b in zip(direct, averaged):
            self.assertAlmostEqual(a, b)

    def test_transparency_depth_order(self):
        red = Card(1., self.full, (1., 0., 0.), .5)
        blue = Card(4., self.full, (0., 0., 1.), .5)
        expected = (.5, 0., .25, .75)
        for cards in ([red, blue], [blue, red]):
            self.assertEqual(trace(self.camera, (0., 0.), (0., 0.), cards), expected)

    def test_hidden_background_is_revealed(self):
        foreground = Card(1., (-.003, .003, -1., 1.), (1., 0., 0.), 1.)
        background = Card(4., self.full, (0., 0., 1.), 1.)
        cards = [foreground, background]
        self.assertEqual(trace(self.camera, (0., 0.), (0., 0.), cards), (1., 0., 0., 1.))
        value = integrate(self.camera, (0., 0.), cards, radius=.025)
        self.assertGreater(value[2], .1)
        self.assertAlmostEqual(value[0] + value[2], 1.)
        self.assertAlmostEqual(value[3], 1.)

    def test_same_sample_occlusion_not_average_alpha(self):
        # Correlated visibility: both cards hit only on the same half-lens.
        cards = [Card(1., (0., 100., -100., 100.), (1., 0., 0.), 1.),
                 Card(1.5, (0., 100., -100., 100.), (0., 0., 1.), 1.)]
        value = integrate(self.camera, (0., 0.), cards)
        self.assertAlmostEqual(value[0], .5)
        self.assertEqual(value[2], 0.)
        # Separately averaging two half-covered layers falsely exposes blue.
        incorrect_blue = .5 * (1 - .5)
        self.assertGreater(incorrect_blue, value[2])

    def test_more_than_sixteen_layers(self):
        cards = [Card(.1 + .02*i, self.full, (1., 1., 1.), .1) for i in range(64)]
        value = trace(self.camera, (0., 0.), (0., 0.), cards)
        expected = 1 - .9**64
        for channel in value:
            self.assertAlmostEqual(channel, expected)

    def test_additive_has_no_opacity(self):
        cards = [Card(1., self.full, (4., 2., 1.), 0., additive=True)]
        self.assertEqual(integrate(self.camera, (0., 0.), cards, rings=2, sectors=8),
                         (4., 2., 1., 0.))

    def test_focused_thin_card_keeps_coverage(self):
        card = Card(2., (-.0001, .0001, -1., 1.), (.3, .2, .1), .4)
        for radius in (0., .01, .1):
            value = integrate(self.camera, (0., 0.), [card], radius, rings=8, sectors=32)
            self.assertAlmostEqual(value[3], .4)

    def test_aperture_convergence(self):
        card = Card(1., (-.003, .003, -1., 1.), (1., 1., 1.), .4)
        # Analytic fraction of a uniform disk intersected by a central strip.
        radius = .025
        q = .003 / ((1 - card.depth / self.camera.focus) * radius)
        expected = .4 * 2 / math.pi * (math.asin(q) + q * math.sqrt(1-q*q))
        for rings, sectors in ((32, 128), (64, 256)):
            actual = integrate(self.camera, (0., 0.), [card], radius, rings, sectors)
            self.assertLess(abs(actual[3] - expected), .001)


if __name__ == "__main__":
    unittest.main(verbosity=2)
