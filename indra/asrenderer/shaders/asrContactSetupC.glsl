/**
 * asrContactSetupC.glsl
 * The contact shadow pass's work list (asrContactShadows.glsl): the light's point on screen,
 * then each sector's bands and the rays that cover the screen in them, as one indirect
 * dispatch. One thread; the light comes from the exposure pass and the shadow fit.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"
#include "asrContactShadows.glsl"

layout(local_size_x = 1) in;

layout(set = 1, binding = 1, r8) uniform writeonly image2D contactMask;

void main()
{
    ViewRef view = ViewRef(push.view);
    ContactShadowsRef work = ContactShadowsRef(view.contactShadows);
    const uint light = ShadowViewsRef(view.shadowViews).light;
    work.light = light;
    work.entryCount = 0u;
    work.dispatchSize = uvec4(0u, 1u, 1u, 0u);
    if (light == SHADOW_LIGHT_NONE || view.shadowCascades == 0u || view.debugView == DEBUG_VIEW_NO_CONTACT)
    {
        return; // the resolve reads no mask
    }

    ExposureRef e = ExposureRef(view.exposure);
    const vec3 toLight = light == SHADOW_LIGHT_SUN ? e.sunDirection : e.moonDirection;
    const vec3 eye = vec3(dot(toLight, view.viewRows[0].xyz), dot(toLight, view.viewRows[1].xyz), dot(toLight, view.viewRows[2].xyz));
    const float along = -eye.z;
    const ivec2 size = ivec2(view.pixelSize.xy);
    // The light's point, from the screen's centre in pixels. Behind the camera, the point
    // in front where the light's opposite projects, and rays run away from it.
    const vec2 across = vec2(eye.x * view.projection.x, -eye.y * view.projection.y) * 0.5 * view.pixelSize.xy;
    const float side = along < 0.0 ? -1.0 : 1.0;
    vec2 offset = across * side / max(abs(along), 1e-20);
    if (length(across) >= CONTACT_LIGHT_LIMIT * abs(along))
    {
        offset = normalize(across) * side * CONTACT_LIGHT_LIMIT;
    }
    const ivec2 lightPixel = ivec2(floor(0.5 * view.pixelSize.xy + offset));
    work.lightPixel = lightPixel;
    work.reverse = along < 0.0 ? 1u : 0u;
    // The point's own pixel lies on no ray.
    if (all(greaterThanEqual(lightPixel, ivec2(0))) && all(lessThan(lightPixel, size)))
    {
        imageStore(contactMask, lightPixel, vec4(1.0));
    }

    uint entries = 0u;
    uint widest = 0u; // the most rays in a band: the dispatch's width
    for (uint sector = 0u; sector < 4u; ++sector)
    {
        const uint axis = sector < 2u ? 0u : 1u;
        const int rows = size[axis];
        const int at = lightPixel[axis];
        // Rows on screen: the pixel at row m is at + m (right, down) or at - m (left, up).
        const int firstRow = (sector & 1u) == 0u ? max(1, -at) : max(1, at - rows + 1);
        const int lastRow = (sector & 1u) == 0u ? rows - 1 - at : at;
        const int acrossLow = -lightPixel[1u - axis];
        const int acrossHigh = size[1u - axis] - 1 - lightPixel[1u - axis];
        for (int band = firstRow; band <= lastRow && entries < CONTACT_MAX_ENTRIES; band += int(CONTACT_GROUP))
        {
            const int outer = band + int(CONTACT_GROUP) - 1;
            const int bandEnd = min(outer, lastRow);
            // The first row where the sector's cone (across within the row) meets the screen;
            // the rays' extremes are there or at the band's end.
            const int nearest = max(band, max(acrossLow, -acrossHigh));
            if (nearest > bandEnd)
            {
                continue;
            }
            int firstRay = 0x7fffffff;
            int lastRay = -0x7fffffff;
            for (int end = 0; end < 2; ++end)
            {
                const int row = end == 0 ? nearest : bandEnd;
                firstRay = min(firstRay, contactOwner(max(acrossLow, -row), row, outer));
                lastRay = max(lastRay, contactOwner(min(acrossHigh, row), row, outer));
            }
            const uint rays = uint(lastRay - firstRay + 1);
            work.entries[entries] = ContactEntry(rays, sector, band, firstRay);
            widest = max(widest, rays);
            ++entries;
        }
    }
    work.entryCount = entries;
    work.dispatchSize = uvec4(entries == 0u ? 0u : widest, entries, 1u, 0u);
}
