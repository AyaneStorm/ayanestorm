/**
 * asrContactShadows.glsl
 * Shared by asrContactSetupC.glsl and asrContactShadowsC.glsl: the work list of the contact
 * shadow pass (Bend Studio's screen-space shadows, 2023, written here from its description).
 *
 * Every ray towards a directional light, seen on screen, runs through the light's point on
 * screen. Around that point the screen is cut into 4 sectors (right, left, down, up: the
 * axis a ray mostly moves along) and each sector into bands of CONTACT_GROUP rows away from
 * the point. One workgroup follows one ray across one band: its threads take one row each,
 * read the depth along the ray once into shared memory, and each thread compares the
 * CONTACT_SAMPLES depths after its own towards the light, so neighbours share their reads.
 * A ray is a line from the light's point, given by its offset across the axis at the band's
 * outer row; rays one pixel apart there are less than a pixel apart nearer the point, so every
 * pixel has one ray that owns it (the nearest) and only that ray writes it: the same mask in
 * every frame, whatever order the groups run in.
 */

const uint CONTACT_GROUP = 128u;      // threads per group, and rows per band
const int CONTACT_SAMPLES = 128;      // depths compared per pixel: 6 cm up to about 0.3 m away
const uint CONTACT_MAX_BANDS = 132u;  // per sector: 16384 rows
const uint CONTACT_MAX_ENTRIES = 4u * CONTACT_MAX_BANDS;
const float CONTACT_BIAS_TEXELS = 3.5; // reach in cascade 0 texels: its bias (1.5 + 1) and a texel for its filter
const float CONTACT_REACH_MAX = 0.06;  // metres towards the light, at most
const float CONTACT_THICKNESS = 0.03; // metres: a surface in the depth hides the ray this far behind it
const float CONTACT_RANGE = 12.0;     // metres along the view; farther, the maps alone
// How far the light's point may sit from the screen, in pixels: past this, the point is
// moved in along the same direction (the rays then turn by under 0.3 degrees on an 8K
// screen), which keeps every product of row and ray within 64-bit integers.
const float CONTACT_LIGHT_LIMIT = 1048576.0;

// One band of one sector: a row of the dispatch, a group per ray from firstRay; the row's
// groups past rayCount stop at once.
struct ContactEntry
{
    uint rayCount;
    uint sector;    // 0 right (+x), 1 left, 2 down (+y), 3 up
    int firstRow;   // the band's first row away from the light's point; it ends CONTACT_GROUP - 1 later
    int firstRay;
};

layout(buffer_reference, std430, buffer_reference_align = 16) buffer ContactShadowsRef
{
    uvec4 dispatchSize;  // the main pass's indirect dispatch: the most rays in a band, by the bands
    ivec2 lightPixel;    // the pixel holding the light's point
    uint reverse;        // 1: the light is behind the camera, rays run away from its point
    uint padWord;
    uint entryCount;
    uint light;          // SHADOW_LIGHT_*, the light traced
    uint pad[2];
    ContactEntry entries[];
};

// floor(a / b) for b > 0, exactly, with no 64-bit division (a long software routine on Apple
// GPUs, where four per thread made the pass 2.5 ms): a float estimate, which is off by at
// most one here (|a / b| stays far under 2^23), corrected with 64-bit products.
// Whether a pixel is within CONTACT_RANGE, from its depth as stored over the near distance
// (1 / distance): the pass writes the mask only there, and the resolve reads it only there.
bool contactInRange(float inverseDistance)
{
    return inverseDistance >= 1.0 / CONTACT_RANGE;
}

int contactFloorDiv(int64_t a, int64_t b)
{
    int64_t q = int64_t(floor(float(a) / float(b)));
    if (q * b > a)
    {
        --q;
    }
    else if ((q + 1) * b <= a)
    {
        ++q;
    }
    return int(q);
}

// The same in 32 bits, for |a| under 2^30: 64-bit products are emulated too, and most frames
// (the light's point within CONTACT_SMALL pixels of the screen) need none.
int contactFloorDiv32(int a, int b)
{
    int q = int(floor(float(a) / float(b)));
    if (q * b > a)
    {
        --q;
    }
    else if ((q + 1) * b <= a)
    {
        ++q;
    }
    return q;
}
const int CONTACT_SMALL = 16000; // rows, rays and offsets under this keep every product in 32 bits

// The offset across the axis of ray `ray` at `row`, rounded to the nearest pixel: the ray
// passes `ray` pixels across at the band's outer row `outer`. Rows read reach outer + 128.
int contactAcross(int ray, int row, int outer)
{
    if (outer < CONTACT_SMALL && abs(ray) < CONTACT_SMALL)
    {
        return contactFloorDiv32(2 * ray * row + outer, 2 * outer);
    }
    return contactFloorDiv(2 * int64_t(ray) * int64_t(row) + int64_t(outer), 2 * int64_t(outer));
}

// The ray owning the pixel `across` at `row`: the nearest one.
int contactOwner(int across, int row, int outer)
{
    if (outer < CONTACT_SMALL && abs(across) < CONTACT_SMALL)
    {
        return contactFloorDiv32(2 * across * outer + row, 2 * row);
    }
    return contactFloorDiv(2 * int64_t(across) * int64_t(outer) + int64_t(row), 2 * int64_t(row));
}

// The pixel at `row` away from the light's point and `across` it, in a sector.
ivec2 contactPixel(ivec2 lightPixel, uint sector, int row, int across)
{
    const int step = (sector & 1u) == 0u ? row : -row;
    return sector < 2u ? lightPixel + ivec2(step, across) : lightPixel + ivec2(across, step);
}
