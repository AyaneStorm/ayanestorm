/**
 * @file asDoFLiveGatherF.glsl
 * @author chanayane@firestorm
 * @brief Live DoF: area-tap scatter-as-gather of one bin at gather
 *        resolution.
 *
 * Every tap reads its bin's mip level matching the tap spacing, so it
 * integrates the area it stands for: no random phase, no noise, the same
 * result at any resolution (scripts/testing/dof_live_reference.py).
 *
 * A tap reads its layer "alone", completed where nearer bins hide it
 * (asDoFLiveCompleteF.glsl, push-pull): the background behind a foreground
 * rock is the background around it, never nothing. Its sources spread energy
 * E = sum w / r^2 over their discs; the tap adds area / unit_area * E times
 * the share of its area the sources reach (liveReach()), r = sqrt(W / E).
 *
 * layer 0 (N2), 1 (N1), 3 (B1): veils, one kernel radius per tile
 *   (asDoFLiveTileF.glsl). B1 also fills behind its own sharper content
 *   (the pixel's own B1, self_* in addTap()).
 * layer 2 (B2): the pixel's own mean radius M / W, completed: nearer, less
 *   blurred surfaces hide the spread of farther ones, and holes take the
 *   radius of the background around them.
 * Cat's eye (cat_eye > 0): the lens barrel clips each source's aperture
 *   (barrelReach()); without darkening its light is renormalized to the open
 *   part, as in the other renderers. The composite applies the darkening.
 * Axial CA (ca_shift > 0): every tap evaluates four wavelength strata
 *   (addTap()); coverage is per channel. The foreground veil keeps it in a
 *   second output (frag_alpha), N1 is written over N2 per channel, and the
 *   composite lays the veil per channel: one (green) alpha lost the fringes
 *   of dark edges over bright areas, its error as large as the fringe
 *   (scripts/testing/dof_live_reference.py, test_axial_ca_edge). B1 is
 *   written over B2 per channel here.
 * Each layer is premultiplied colour and coverage. Layers 1 and 3 are drawn
 * after 0 and 2 and write the pair the composite reads: N1 is written over
 * N2 (one foreground veil), B1 over B2 normalized (the background).
 */

layout(location = 0) out vec4 frag_color;
// Per-channel coverage (rgb) of N2, then of the foreground veil (axial CA).
layout(location = 1) out vec4 frag_alpha;

// The layer alone: N2 raw, the others completed (asDoFLiveCompleteF.glsl).
uniform sampler2D diffuseRect;   // the bin's (S.rgb, W), mipmapped
// N2: raw (E_N2, ...); others: completed (E_N1, E_B1, M_B2), mipmapped.
uniform sampler2D specularRect;
uniform sampler2D bloomMap;      // F (S.rgb, W), raw: skip in-focus pixels
uniform sampler2D noiseMap;      // tile kernel radii (N2, N1, B1)
uniform sampler2D lightMap;      // layer 1: the N2 veil; layer 3: B2
uniform sampler2D normalMap;     // layer 1, axial CA: N2's per-channel coverage
uniform vec2 target_res;
uniform int layer;
uniform int max_rings;
// Composite debug views 3, 9 and 10 (asDoFLiveCompositeF.glsl): the B1
// pass writes the tile kernel radii, B1 alone, or B2 alone, instead of B1
// over B2.
uniform int debug_mode;
// Axial CA: blur shift of the extreme wavelengths, normalized CoC; 0 off.
uniform float ca_shift;
uniform float near_radius;
uniform float far_radius;
uniform float far_split_radius;
uniform float gather_scale;

const int LAYER_N2 = 0;
const int LAYER_N1 = 1;
const int LAYER_B2 = 2;
const int LAYER_B1 = 3;
const int TILE = 8;
const int MAX_RINGS = 7;
const float LIVE_PI_G = 3.14159265358979323846;

#ifdef LIVE_TAP_TABLE
// Ring tap geometry, the same for every pixel (asdoflive.cpp,
// uploadTapTable()): unit offset (x, y), boundary, sector area span. Ring
// k's taps start at 3 k (k - 1). Without it (a driver refusing 672 more
// uniform components), the taps compute the same geometry themselves.
uniform vec4 live_taps[3 * MAX_RINGS * (MAX_RINGS + 1)];
#endif

uniform float unit_area;
uniform float anamorphic_ratio;
// Cat's-eye barrel shift at the frame corner, aperture radii; 0 off.
uniform float cat_eye;
vec2 liveTapOffset(float angle, float distance, out float boundary);
float liveApertureAreaTo(float angle);
vec2 liveFieldPosition(vec2 uv);
float liveReach(float r, float d, float s, float sa);
float liveSphericalProduct(float r, bool background);
float liveFarEnergy(float weight, float moment);

// The layer alone at uv and lod, and its energies.
bool readLayer(vec2 uv, float lod, out vec4 value, out vec4 energies)
{
    value = textureLod(diffuseRect, uv, lod);
    energies = textureLod(specularRect, uv, lod);
    return value.a > 0.000001;
}

bool readTap(vec2 uv, float lod, out vec4 layer_value, out float energy)
{
    vec4 energies;
    if (!readLayer(uv, lod, layer_value, energies))
    {
        return false;
    }
    // N2: raw .x; N1, B1, B2: completed .x, .y, .z (M_B2).
    energy = layer == LAYER_N2 || layer == LAYER_N1 ? energies.x :
        (layer == LAYER_B1 ? energies.y : liveFarEnergy(layer_value.a, energies.z));
    return layer_value.a > 0.00001 && energy > 0.0;
}

// Cat's eye (mechanical vignetting), as in the other renderers: a pupil
// point p of a source's unit aperture (rotation, polygon and squeeze
// included; p = tap offset / r) passes inside the lens barrel, a unit circle
// centred at cat_eye * field, the shift capped at 1.6 radii.
//
// In the area-uniform angle A = liveApertureAreaTo(theta) and u = tau^2 (tau
// the pupil radius over the boundary), every aperture is the rectangle
// [0, unit_area] x [0, 1] and every tap an exact sub-rectangle; the open
// part is a band u1(A) <= u <= u2(A) along each ray. The band is sampled at
// BARREL_NODES angles per pixel and is linear in A between them. Every tap
// and the open fraction of the aperture integrate that same band exactly
// (the spherical profile included), so a uniform field keeps coverage 1 for
// any kernel, ring count, source radius and aperture shape: sampling the
// clip per tap instead left several percent that changed with the tile's
// kernel (scripts/testing/dof_live_reference.py, BarrelBand).
const int BARREL_NODES = 24;
const float BARREL_MAX_SHIFT = 1.6;
bool barrel_on = false;
// Open fraction for the last |sa| > 1 (barrelReach()).
float barrel_cut_sa = 0.0;
float barrel_cut_fraction = 1.0;
float barrel_lo[BARREL_NODES];
float barrel_hi[BARREL_NODES];
float barrel_area[BARREL_NODES + 1];
vec2 barrel_fraction;  // open fraction of the aperture: x + sa * y

// Mean over a linear ramp l0..l1 of (c, c - c^2), c = clamp(l, a, b): the
// moments of the spherical profile's antiderivative u + sa (u - u^2).
vec2 rampMean(float l0, float l1, float a, float b)
{
    b = max(b, a);
    float lo_v = min(l0, l1);
    float hi_v = max(l0, l1);
    float span = hi_v - lo_v;
    if (span < 1e-6)
    {
        float c = clamp(l0, a, b);
        return vec2(c, c - c * c);
    }
    float pa = clamp((a - lo_v) / span, 0.0, 1.0);
    float pb = clamp((hi_v - b) / span, 0.0, 1.0);
    float pm = max(1.0 - pa - pb, 0.0);
    float x0 = max(lo_v, a);
    float x1 = max(min(hi_v, b), x0);
    float mean = 0.5 * (x0 + x1);
    float sq = (x0 * x0 + x0 * x1 + x1 * x1) / 3.0;
    return vec2(pa * a + pb * b + pm * mean,
                pa * (a - a * a) + pb * (b - b * b) + pm * (mean - sq));
}

// Moments of the band over A in [a0, a1] (the sector theta0..theta1) and u
// in [ua, ub].
vec2 barrelMoments(float a0, float a1, float theta0, float theta1, float ua, float ub)
{
    float node_step = 2.0 * LIVE_PI_G / float(BARREL_NODES);
    int j0 = int(floor(theta0 / node_step));
    int j1 = int(floor(theta1 / node_step));
    vec2 moments = vec2(0.0);
    for (int i = 0; i <= BARREL_NODES + 1; ++i)
    {
        int j = j0 + i;
        if (j > j1)
        {
            break;
        }
        float wraps = floor(float(j) / float(BARREL_NODES));
        int jj = j - int(wraps) * BARREL_NODES;
        int k = jj + 1 < BARREL_NODES ? jj + 1 : 0;
        float s0 = barrel_area[jj] + wraps * unit_area;
        float s1 = barrel_area[jj + 1] + wraps * unit_area;
        float x0 = max(s0, a0);
        float x1 = min(s1, a1);
        if (x1 <= x0)
        {
            continue;
        }
        float t0 = (x0 - s0) / (s1 - s0);
        float t1 = (x1 - s0) / (s1 - s0);
        vec2 upper = rampMean(mix(barrel_hi[jj], barrel_hi[k], t0),
                              mix(barrel_hi[jj], barrel_hi[k], t1), ua, ub);
        vec2 lower = rampMean(mix(barrel_lo[jj], barrel_lo[k], t0),
                              mix(barrel_lo[jj], barrel_lo[k], t1), ua, ub);
        moments += (x1 - x0) * (upper - lower);
    }
    return moments;
}

// The band of this gather pixel.
void setupBarrel(vec2 uv)
{
    barrel_on = cat_eye > 0.0;
    if (!barrel_on)
    {
        return;
    }
    vec2 barrel = cat_eye * liveFieldPosition(uv);
    float shift = length(barrel);
    if (shift > BARREL_MAX_SHIFT)
    {
        barrel *= BARREL_MAX_SHIFT / shift;
    }
    float c = dot(barrel, barrel) - 1.0;
    for (int j = 0; j <= BARREL_NODES; ++j)
    {
        float theta = 2.0 * LIVE_PI_G * float(j) / float(BARREL_NODES);
        barrel_area[j] = liveApertureAreaTo(theta);
        if (j == BARREL_NODES)
        {
            break;
        }
        float boundary;
        vec2 v = liveTapOffset(theta, 1.0, boundary);
        float a = dot(v, v);
        float b = dot(v, barrel);
        float disc = b * b - a * c;
        // A ray that misses the barrel holds an empty band, continuous with
        // the tangent ray's.
        float q = sqrt(max(disc, 0.0));
        float t1 = (b - q) / a;
        float t2 = (b + q) / a;
        barrel_lo[j] = max(t1, 0.0) * max(t1, 0.0);
        barrel_hi[j] = max(t2, 0.0) * max(t2, 0.0);
    }
    barrel_fraction = barrelMoments(barrel_area[0], barrel_area[BARREL_NODES], 0.0,
                                    2.0 * LIVE_PI_G * (1.0 - 0.5 / float(BARREL_NODES)),
                                    0.0, 1.0) / unit_area;
}

// liveReach() through the barrel, compensated: the share of the tap's area
// (A in [a0, a1], the sector theta0..theta1) the source reaches through the
// barrel, over the open fraction of its aperture.
float barrelReach(float r, float d, float s, float sa, vec4 sector)
{
    float lo = d <= 0.0 ? 0.0 : (d - 0.5 * s) * (d - 0.5 * s);
    float hi = (d + 0.5 * s) * (d + 0.5 * s);
    float r2 = max(r * r, 1e-8);
    float ua = min(lo / r2, 1.0);
    float ub = min(hi / r2, 1.0);
    if (ua >= 1.0)
    {
        return 0.0;
    }
    float open_fraction = barrel_fraction.x + sa * barrel_fraction.y;
    if (abs(sa) > 1.0)
    {
        // The spherical profile cut where it turns negative
        // (liveSphericalNorm()); the open fraction is that cut profile's,
        // over the same band, kept for the last strength.
        float cut = (1.0 + sa) / (2.0 * sa);
        vec2 positive = sa > 0.0 ? vec2(0.0, cut) : vec2(cut, 1.0);
        ua = max(ua, positive.x);
        ub = min(ub, positive.y);
        if (ub <= ua)
        {
            return 0.0;
        }
        if (sa != barrel_cut_sa)
        {
            vec2 whole = barrelMoments(barrel_area[0], barrel_area[BARREL_NODES], 0.0,
                                       2.0 * LIVE_PI_G * (1.0 - 0.5 / float(BARREL_NODES)),
                                       positive.x, positive.y);
            barrel_cut_sa = sa;
            barrel_cut_fraction = (whole.x + sa * whole.y) / unit_area;
        }
        open_fraction = barrel_cut_fraction;
    }
    open_fraction = max(open_fraction, 0.01);
    vec2 moments = barrelMoments(sector.x, sector.y, sector.z, sector.w, ua, ub);
    return max(moments.x + sa * moments.y, 0.0) /
           ((sector.y - sector.x) * (hi - lo) / r2) / open_fraction;
}

// B1 self-occlusion. Behind the focus, sharper content is nearer: a tap
// whose B1 is sharper than this pixel's own hides B1 behind it that no bin
// stores (completion runs only for nearer bins). The jaw edge, partly B1 on
// the focus ramp, over a more blurred B1 neck left the far background
// showing through in a light line below the jaw (thin magenta lines in
// debug view 9). Each such tap's hidden share is what content like the
// pixel's own B1 (density, radius) would add over the part of the tap its
// own content does not reach; gatherLayer() fills the coverage deficit with
// the pixel's colour, up to that sum. Equal radii, smooth blur ramps
// (coverage already 1) and taps without B1 (a true edge over the far
// background) add nothing (scripts/testing/dof_live_reference.py,
// evaluate_self_occlusion()).
bool self_on = false;
float self_weight;  // the pixel's own B1 density W_p
float self_radius;  // and radius r_p
float self_sa;
vec3 self_hidden;

// Axial chromatic aberration, the other renderers' spectral model
// (ASDoFAperture::spectralWeights): wavelength s blurs to radius r - sigma delta s
// (sigma +1 behind the focus, -1 in front; red, s > 0, focuses farther),
// four strata of s, channel weights red 1 + s, green 1.5 (1 - s^2), blue
// 1 - s, each summing to 1. Each stratum has its own exact reach and keeps
// the source's energy (W / r_k^2), so a uniform field keeps coverage 1 in
// every channel; the kernels span the widest stratum (+ 0.75 delta, the
// tile pass and farKernel()).
const vec4 CA_STRATA = vec4(-0.75, -0.25, 0.25, 0.75);
const vec4 CA_RED = vec4(0.0625, 0.1875, 0.3125, 0.4375);
const vec4 CA_GREEN = vec4(0.1590909, 0.3409091, 0.3409091, 0.1590909);
const vec4 CA_BLUE = vec4(0.4375, 0.3125, 0.1875, 0.0625);
bool ca_on = false;
float ca_sigma_delta;  // sigma delta, gather pixels

float caStratumRadius(float r, int k)
{
    return max(abs(r - ca_sigma_delta * CA_STRATA[k]), 0.5);
}

// One tap: adds its colour and weight. d and s are aperture-space distance
// and spacing (the reach test); spacing is the image-space tap spacing (the
// mip level); sector is the tap's (A0, A1, theta0, theta1) for the barrel.
// Taps past the frame read its edge: content continues there, so a
// foreground touching the frame keeps its coverage.
void addTap(vec2 center, vec2 offset, float d, float s, float spacing,
            float area, vec4 sector, inout vec3 color_sum, inout vec3 weight_sum)
{
    vec2 uv = clamp((center + offset) / target_res, vec2(0.0), vec2(1.0));
    float lod = log2(max(spacing, 1.0));
    vec4 value;
    float energy;
    if (!readTap(uv, lod, value, energy))
    {
        return;
    }
    float r = sqrt(value.a / energy);
    bool background = layer == LAYER_B2 || layer == LAYER_B1;
    if (!ca_on)
    {
        float sa = liveSphericalProduct(r, background);
        float reach = barrel_on ? barrelReach(r, d, s, sa, sector) : liveReach(r, d, s, sa);
        float weight = area / unit_area * energy * reach;
        color_sum += value.rgb / value.a * weight;
        weight_sum += vec3(weight);
        if (self_on && r < self_radius)
        {
            float reach_own = barrel_on ? barrelReach(self_radius, d, s, self_sa, sector) :
                                          liveReach(self_radius, d, s, self_sa);
            self_hidden += vec3(area / unit_area * value.a * self_weight /
                                (self_radius * self_radius) * max(reach_own - reach, 0.0));
        }
        return;
    }
    vec3 rgb = value.rgb / value.a;
    for (int k = 0; k < 4; ++k)
    {
        vec3 channels = vec3(CA_RED[k], CA_GREEN[k], CA_BLUE[k]);
        float r_k = caStratumRadius(r, k);
        float sa = liveSphericalProduct(r_k, background);
        float reach = barrel_on ? barrelReach(r_k, d, s, sa, sector) : liveReach(r_k, d, s, sa);
        vec3 weight = area / unit_area * value.a / (r_k * r_k) * reach * channels;
        color_sum += rgb * weight;
        weight_sum += weight;
        float own_k = self_on ? caStratumRadius(self_radius, k) : 0.0;
        if (self_on && r_k < own_k)
        {
            float own_sa = liveSphericalProduct(own_k, true);
            float reach_own = barrel_on ? barrelReach(own_k, d, s, own_sa, sector) :
                                          liveReach(own_k, d, s, own_sa);
            self_hidden += area / unit_area * value.a * self_weight / (own_k * own_k) *
                           max(reach_own - reach, 0.0) * channels;
        }
    }
}

// Far kernel radius: the completed mean radius M / W here; 0 where no far
// background exists at all.
float farKernel(vec2 uv)
{
    vec4 value;
    vec4 energies;
    if (!readLayer(uv, 0.0, value, energies))
    {
        return 0.0;
    }
    return energies.z / value.a;
}

// This layer at this pixel: premultiplied colour and coverage, and the
// coverage per channel (axial CA; the coverage in every channel otherwise).
vec4 gatherLayer(vec2 center, out vec3 alpha)
{
    alpha = vec3(0.0);
    vec2 uv = center / target_res;
    float kernel;
    bool background = layer == LAYER_B2 || layer == LAYER_B1;
    // Fully in focus here: F hides whatever the background would hold.
    if (background && textureLod(bloomMap, uv, 0.0).a >= 0.999)
    {
        return vec4(0.0);
    }
    if (layer == LAYER_B2)
    {
        // The tile pass adds the widest stratum to the veil kernels.
        kernel = farKernel(uv);
        kernel = ca_on && kernel > 0.0 ? kernel + 0.75 * abs(ca_sigma_delta) : kernel;
    }
    else
    {
        vec3 radii = texelFetch(noiseMap, ivec2(center) / TILE, 0).xyz;
        kernel = layer == LAYER_N2 ? radii.x : (layer == LAYER_N1 ? radii.y : radii.z);
    }
    // Skip only where the bin holds nothing (kernel 0). Every source's
    // radius is at least half a gather pixel (liveDecompose()), so content
    // just off the focus sits exactly at 0.5: a "kernel < 0.5" test skipped
    // it whenever 16-bit rounding of W / E fell a hair below, tile by tile
    // (the far background showed through in blocks). Such content is read
    // nearly sharp, by the centre taps.
    if (kernel <= 0.0)
    {
        return vec4(0.0);
    }
    kernel = max(kernel, 0.5);

    int rings = clamp(int(ceil(kernel - 0.5)), 1, min(max_rings, MAX_RINGS));
    float s = kernel / (float(rings) + 0.5);
    // Foreground sources image as the inverted aperture (the source reaching
    // this pixel lies at +offset), background ones upright (-offset), as in
    // the aperture-sampled renderer (ASDoFCamera).
    float direction = background ? -1.0 : 1.0;
    vec3 color_sum = vec3(0.0);
    vec3 weight_sum = vec3(0.0);
    float squeeze = max(anamorphic_ratio, 1.0);
    // The pixel's own B1, completed (self_*, hidden in addTap()).
    vec4 self_color = vec4(0.0);
    self_on = false;
    self_hidden = vec3(0.0);
    if (layer == LAYER_B1)
    {
        vec4 own;
        vec4 own_energies;
        if (readLayer(uv, 0.0, own, own_energies) && own.a > 0.001 && own_energies.y > 0.0)
        {
            self_on = true;
            self_weight = own.a;
            self_radius = sqrt(own.a / own_energies.y);
            self_sa = liveSphericalProduct(self_radius, true);
            self_color = vec4(own.rgb / own.a, 1.0);
        }
    }
    setupBarrel(uv);
    vec4 centre_sector = barrel_on ?
        vec4(liveApertureAreaTo(-LIVE_PI_G), liveApertureAreaTo(LIVE_PI_G), -LIVE_PI_G, LIVE_PI_G) :
        vec4(0.0);
    addTap(center, vec2(0.0), 0.0, s, s * squeeze, unit_area * 0.25 * s * s,
           centre_sector, color_sum, weight_sum);
    for (int k = 1; k <= MAX_RINGS; ++k)
    {
        if (k > rings)
        {
            break;
        }
        int count = 6 * k;
        float offset = (k & 1) != 0 ? 0.5 : 0.0;
        float d = float(k) * s;
        for (int j = 0; j < 6 * MAX_RINGS; ++j)
        {
            if (j >= count)
            {
                break;
            }
            float angle = 2.0 * LIVE_PI_G * (float(j) + offset) / float(count);
            float half_angle = LIVE_PI_G / float(count);
#ifdef LIVE_TAP_TABLE
            vec4 entry = live_taps[3 * k * (k - 1) + j];
            vec2 tap = entry.xy * d;
            float boundary = entry.z;
            float area = 2.0 * d * s * entry.w;
            // Only the barrel reads the sector bounds; it keeps its own
            // areas, unchanged.
            vec4 sector = vec4(0.0);
            if (barrel_on)
            {
                sector = vec4(liveApertureAreaTo(angle - half_angle),
                              liveApertureAreaTo(angle + half_angle),
                              angle - half_angle, angle + half_angle);
                area = 2.0 * d * s * (sector.y - sector.x);
            }
#else
            float boundary;
            vec2 tap = liveTapOffset(angle, d, boundary);
            // Integrate the whole angular sector, not boundary^2 at its
            // midpoint: every ring must partition its annulus exactly
            // (liveApertureAreaTo()). The radial squared-width is 2*d*s;
            // squeeze is in the area.
            vec4 sector = vec4(liveApertureAreaTo(angle - half_angle),
                               liveApertureAreaTo(angle + half_angle),
                               angle - half_angle, angle + half_angle);
            float area = 2.0 * d * s * (sector.y - sector.x);
#endif
            addTap(center, tap * direction, d, s, s * boundary * squeeze, area,
                   sector, color_sum, weight_sum);
        }
    }

    // Premultiplied for every layer: skipped pixels are (0, 0, 0, 0), and the
    // composite's bilinear upsampling must not mix that black into the
    // colour of their neighbours (dark fringes along in-focus strands).
    if (!ca_on)
    {
        float coverage = clamp(weight_sum.g, 0.0, 1.0);
        vec4 result = weight_sum.g > 0.0 ? vec4(color_sum / weight_sum.g * coverage, coverage) :
                                           vec4(0.0);
        if (self_on)
        {
            result += self_color * min(1.0 - coverage, self_hidden.g);
        }
        alpha = vec3(result.a);
        return result;
    }
    vec3 coverage = clamp(weight_sum, 0.0, 1.0);
    vec3 premultiplied = color_sum / max(weight_sum, vec3(1e-20)) * coverage;
    if (self_on)
    {
        vec3 fill = min(1.0 - coverage, self_hidden);
        premultiplied += self_color.rgb * fill;
        coverage += fill;
    }
    alpha = coverage;
    if (layer == LAYER_B2)
    {
        // Normalized by the composite with one alpha: colour per channel.
        return vec4(premultiplied / max(coverage, vec3(1e-6)) * coverage.g, coverage.g);
    }
    return vec4(premultiplied, coverage.g);
}

void main()
{
    vec2 center = gl_FragCoord.xy;
    bool background = layer == LAYER_B2 || layer == LAYER_B1;
    float delta = ca_shift * (background ? far_radius : near_radius) * gather_scale;
    ca_on = delta > 0.0;
    ca_sigma_delta = background ? delta : -delta;
    vec3 alpha;
    vec4 result = gatherLayer(center, alpha);
    if (layer == LAYER_N1)
    {
        // N2 in front of N1.
        vec4 near2 = texelFetch(lightMap, ivec2(center), 0);
        if (!ca_on)
        {
            result = near2 + result * (1.0 - near2.a);
            alpha = vec3(result.a);
        }
        else
        {
            vec3 near2_alpha = texelFetch(normalMap, ivec2(center), 0).rgb;
            result = vec4(near2.rgb + result.rgb * (1.0 - near2_alpha),
                          near2.a + result.a * (1.0 - near2.a));
            alpha = near2_alpha + alpha * (1.0 - near2_alpha);
        }
    }
    else if (layer == LAYER_B1)
    {
        // B1 in front of B2, which is the farthest content: normalized, it
        // fills whatever B1 leaves uncovered (per channel with axial CA).
        vec4 back2 = texelFetch(lightMap, ivec2(center), 0);
        if (debug_mode == 10)
        {
            result = back2;
        }
        else if (back2.a > 0.0001 && debug_mode != 9)
        {
            if (!ca_on)
            {
                result += vec4(back2.rgb / back2.a, 1.0) * (1.0 - result.a);
            }
            else
            {
                result = vec4(result.rgb + back2.rgb / back2.a * (1.0 - alpha),
                              result.a + (1.0 - result.a));
            }
        }
        if (debug_mode == 3)
        {
            // Tile kernel radii relative to each bin's largest: N2 red, N1
            // green, B1 blue. A gathered tile shows at least a quarter
            // brightness, so the smallest kernels (half a gather pixel)
            // stand apart from skipped tiles (black).
            vec3 radii = texelFetch(noiseMap, ivec2(center) / TILE, 0).xyz;
            vec3 relative = clamp(vec3(radii.xy / max(0.5 * near_radius, 0.5),
                                       radii.z / max(0.625 * far_split_radius, 0.5)), 0.0, 1.0);
            result = vec4(mix(vec3(0.0), 0.25 + 0.75 * relative, greaterThan(radii, vec3(0.0))), 1.0);
        }
    }
    frag_color = result;
    frag_alpha = vec4(alpha, 1.0);
}
