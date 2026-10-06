/**
 * @file asstars.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local star catalogue: procedural (density, blackbody colors,
 * size, twinkle) or real sky from a magnitude-sorted catalogue. Defaults
 * reproduce the stock LLVOWLSky stars.
 */

#include "llviewerprecompiledheaders.h"

#include "asstars.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrand.h"
#include "llsky.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llvowlsky.h"
#include "v3color.h"

namespace
{
    // Stock draws getStarsNumVerts()*4 vertices of a 6-vertex-per-star
    // buffer, so only 666 of its 1000 stars are ever visible.
    constexpr U32 STOCK_VISIBLE_STARS = 666;
    constexpr U32 MAX_STARS = 20000;
    // Extra band stars at full Milky Way concentration, as a multiple of the
    // background count.
    constexpr F32 MILKY_WAY_EXTRA_STARS = 1.5f;

    // Real sky: per-star brightness relative to the stock star level
    // (which the shader's x32 already pushes past white). The faintest
    // drawn star sits at REAL_SKY_FAINT_LEVEL; brightness is stored in
    // vertex alpha as log2 over REAL_SKY_LOG_RANGE octaves (8-bit alpha
    // cannot hold the ~1:1000 range linearly) and decoded in starsF.glsl.
    // Brightness above the stock level goes into sprite area.
    constexpr F32 REAL_SKY_BASE_SIZE = 18.f;
    constexpr F32 REAL_SKY_FAINT_LEVEL = 1.f / 32.f;
    // Real sky: stars per unit of density; 1 draws the 6660 brightest
    // (about the naked-eye limit, magnitude 6.3), 3 the whole catalogue.
    constexpr U32 REAL_SKY_STARS_PER_DENSITY = 6660;
    constexpr F32 REAL_SKY_LOG_RANGE = 8.f;
    constexpr F32 REAL_SKY_MAX_SIZE_SCALE = 4.f;

    // Settings that change the catalogue and therefore need a rebuild.
    const std::vector<std::string> sCatalogueControls = {
        "ASStarsDensity", "ASStarsColorAmount", "ASStarsSaturation",
        "ASStarsTemperatureBias", "ASStarsBrightnessVariation", "ASStarsSize",
        "ASStarsMilkyWay", "ASStarsSeed", "ASStarsEnabled",
        "ASStarsMode", "ASStarsMagnitudeContrast"
    };
    // Every resettable setting of the panel.
    const std::vector<std::string> sAllControls = {
        "ASStarsDensity", "ASStarsColorAmount", "ASStarsSaturation",
        "ASStarsTemperatureBias", "ASStarsBrightness", "ASStarsBrightnessVariation",
        "ASStarsSize", "ASStarsTwinkle", "ASStarsMilkyWay", "ASStarsSeed",
        "ASStarsMode", "ASStarsLatitude", "ASStarsSiderealOffset",
        "ASStarsMagnitudeContrast", "ASMilkyWayEnabled", "ASMilkyWayIntensity",
        "ASMilkyWayDeepSkyIntensity", "ASMilkyWaySaturation"
    };

    U32 sCount = 0;
    std::vector<F32> sSizes;
    // Latched by generate(): the stars are in the equatorial frame.
    bool sRealSky = false;
    // Latched by generate(): vertex alpha holds log-encoded brightness
    // (real sky, or procedural with brightness contrast > 0).
    bool sEncoded = false;

    // Real-sky catalogue (app_settings/stars/as_star_catalog.bin, built by
    // scripts/content_tools/as_build_star_catalog.py), brightest first.
    struct CatalogueStar
    {
        LLVector3 mDir;  // J2000 equatorial unit vector, z = north pole
        F32 mMag;        // apparent magnitude
        F32 mBV;         // B-V color index
        bool mHasBV;
    };
    std::vector<CatalogueStar> sCatalogue;
    bool sCatalogueLoaded = false;

    // Loads the catalogue once; false (empty catalogue) when missing or
    // malformed, in which case the procedural sky is used.
    bool loadCatalogue()
    {
        if (sCatalogueLoaded)
        {
            return !sCatalogue.empty();
        }
        sCatalogueLoaded = true;

        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "stars", "as_star_catalog.bin");
        llifstream file(path.c_str(), std::ios::in | std::ios::binary);
        char magic[8] = {};
        U32 header[2] = {};
        if (!file.is_open()
            || !file.read(magic, sizeof(magic))
            || memcmp(magic, "ASSTAR01", sizeof(magic)) != 0
            || !file.read(reinterpret_cast<char*>(header), sizeof(header))
            || header[0] > 1000000)
        {
            LL_WARNS("ASStars") << "Star catalogue missing or invalid: " << path << LL_ENDL;
            return false;
        }

        // Records: int16 x, y, z (* 32767), mag (* 1000), bv (* 1000 or -32768).
        std::vector<S16> records((size_t)header[0] * 5);
        if (!file.read(reinterpret_cast<char*>(records.data()), records.size() * sizeof(S16)))
        {
            LL_WARNS("ASStars") << "Star catalogue truncated: " << path << LL_ENDL;
            return false;
        }

        sCatalogue.resize(header[0]);
        for (U32 i = 0; i < header[0]; ++i)
        {
            const S16* r = &records[(size_t)i * 5];
            CatalogueStar& star = sCatalogue[i];
            star.mDir.set(r[0] / 32767.f, r[1] / 32767.f, r[2] / 32767.f);
            star.mDir.normVec();
            star.mMag = r[3] / 1000.f;
            star.mHasBV = r[4] != -32768;
            star.mBV = star.mHasBV ? r[4] / 1000.f : 0.f;
        }
        LL_INFOS("ASStars") << "Loaded " << sCatalogue.size() << " catalogue stars" << LL_ENDL;
        return true;
    }

    // B-V color index to effective temperature (Ballesteros 2012).
    F32 bvToTemperature(F32 bv)
    {
        bv = llclamp(bv, -0.4f, 2.0f);
        return 4600.f * (1.f / (0.92f * bv + 1.7f) + 1.f / (0.92f * bv + 0.62f));
    }

    // Rough naked-eye spectral class mix: temperature range (K) and weight.
    struct SpectralClass { F32 mMinK; F32 mMaxK; F32 mWeight; };
    const SpectralClass sClasses[] = {
        { 10000.f, 30000.f, 0.12f },  // O/B blue-white
        {  7500.f, 10000.f, 0.20f },  // A white
        {  6000.f,  7500.f, 0.15f },  // F yellow-white
        {  5200.f,  6000.f, 0.15f },  // G yellow
        {  3700.f,  5200.f, 0.30f },  // K orange (incl. giants)
        {  2400.f,  3700.f, 0.08f },  // M red
    };

    F32 sampleTemperature(std::mt19937& rng)
    {
        std::uniform_real_distribution<F32> uni(0.f, 1.f);
        F32 pick = uni(rng);
        for (const SpectralClass& c : sClasses)
        {
            if (pick < c.mWeight || &c == &sClasses[LL_ARRAY_SIZE(sClasses) - 1])
            {
                // Log-uniform inside the class.
                return c.mMinK * std::pow(c.mMaxK / c.mMinK, uni(rng));
            }
            pick -= c.mWeight;
        }
        return 6500.f;
    }

    F32 srgbToLinear(F32 c)
    {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    // Blackbody color (analytic sRGB fit, 1000-40000 K), converted to linear
    // and normalised so the brightest channel is 1; star intensity stays in
    // the alpha channel.
    LLColor3 blackbody(F32 kelvin)
    {
        const F32 t = llclamp(kelvin, 1000.f, 40000.f) / 100.f;
        F32 r, g, b;
        if (t <= 66.f)
        {
            r = 255.f;
            g = 99.4708025861f * std::log(t) - 161.1195681661f;
        }
        else
        {
            r = 329.698727446f * std::pow(t - 60.f, -0.1332047592f);
            g = 288.1221695283f * std::pow(t - 60.f, -0.0755148492f);
        }
        if (t >= 66.f)
        {
            b = 255.f;
        }
        else if (t <= 19.f)
        {
            b = 0.f;
        }
        else
        {
            b = 138.5177312231f * std::log(t - 10.f) - 305.0447927307f;
        }
        LLColor3 col(srgbToLinear(llclamp(r, 0.f, 255.f) / 255.f),
                     srgbToLinear(llclamp(g, 0.f, 255.f) / 255.f),
                     srgbToLinear(llclamp(b, 0.f, 255.f) / 255.f));
        const F32 peak = llmax(col.mV[VRED], llmax(col.mV[VGREEN], col.mV[VBLUE]));
        return peak > 0.f ? col * (1.f / peak) : LLColor3::white;
    }

    // Upper-hemisphere direction concentrated around a fixed tilted great
    // circle (the band); the circle is symmetric, so flipping keeps it on it.
    LLVector3 sampleBandDirection(std::mt19937& rng)
    {
        static const LLVector3 band_normal = []()
        {
            LLVector3 n(0.35f, -0.6f, 0.72f);
            n.normVec();
            return n;
        }();
        static const LLVector3 band_u = []()
        {
            LLVector3 u = band_normal % LLVector3(0.f, 0.f, 1.f);
            u.normVec();
            return u;
        }();
        static const LLVector3 band_v = band_normal % band_u;

        std::uniform_real_distribution<F32> angle(0.f, F_TWO_PI);
        std::normal_distribution<F32> spread(0.f, 0.12f);
        const F32 theta = angle(rng);
        LLVector3 dir = band_u * std::cos(theta) + band_v * std::sin(theta) + band_normal * spread(rng);
        dir.normVec();
        if (dir.mV[VZ] < 0.f)
        {
            dir = -dir;
        }
        return dir;
    }

    // Real-sky orientation. Stars are in the equatorial frame (z = north
    // celestial pole); the local frame is x east, y north, z up. Spin by the
    // local sidereal time around the pole, then tilt the pole up to the
    // observer's latitude above the north horizon. Free running at the stock
    // rate. Shared by the star dome and the Milky Way glow.
    void realSkyAngles(F32 star_time, F32& tilt_deg, F32& spin_deg)
    {
        static LLCachedControl<F32> latitude(gSavedSettings, "ASStarsLatitude", 45.f);
        static LLCachedControl<F32> sidereal_offset(gSavedSettings, "ASStarsSiderealOffset", 0.f);
        const F32 lst_deg = star_time * 0.01f + (F32)sidereal_offset * 15.f;
        // Tilt about east: maps the pole (0,0,1) to (0, cos lat, sin lat).
        tilt_deg = -(90.f - llclamp((F32)latitude, -90.f, 90.f));
        // Spin about the pole: hour angle 0 (RA = LST) lands on the south
        // meridian.
        spin_deg = -(lst_deg + 90.f);
    }

    // Derived panel enable states (ASStarsProceduralUI / ASStarsRealSkyUI).
    void updatePanelStates()
    {
        const bool on = gSavedSettings.getBOOL("ASStarsEnabled");
        const S32 mode = gSavedSettings.getS32("ASStarsMode");
        gSavedSettings.setBOOL("ASStarsProceduralUI", on && mode != 1);
        gSavedSettings.setBOOL("ASStarsRealSkyUI", on && mode == 1);
    }

    void requestRebuild()
    {
        if (gSky.mVOWLSkyp.notNull())
        {
            gSky.mVOWLSkyp->asRebuildStars();
        }
    }
}

void ASStars::registerUICallbacks()
{
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASStars.RandomizeSeed",
        [](LLUICtrl*, const LLSD&) { gSavedSettings.setS32("ASStarsSeed", 1 + ll_rand(999999)); });
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASStars.ResetDefault",
        [](LLUICtrl*, const LLSD& data)
        {
            const std::string control_name = data.asString();
            for (const std::string& name : sAllControls)
            {
                // "all" resets the whole panel.
                if (control_name == "all" || control_name == name)
                {
                    if (LLControlVariable* control = gSavedSettings.getControl(name))
                    {
                        control->resetToDefault(true);
                    }
                }
            }
        });

    for (const std::string& name : sCatalogueControls)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { requestRebuild(); });
        }
    }

    // Panel enable states: enabled_control takes a single boolean, so keep
    // derived ones for "enabled and procedural" / "enabled and real sky".
    for (const char* name : { "ASStarsEnabled", "ASStarsMode" })
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { updatePanelStates(); });
        }
    }
    updatePanelStates();
}

void ASStars::generate(F32 radius, std::vector<LLVector3>& positions,
                       std::vector<LLColor4>& colors, std::vector<F32>& intensities)
{
    // Read settings directly, not through LLCachedControl: generate() runs
    // from the settings' own change signals (connected first, at startup),
    // which fire before a cached control would have picked up the new value.
    // Master toggle off: the stock parameters, whatever the panel holds.
    const bool on = gSavedSettings.getBOOL("ASStarsEnabled");
    const F32 density = on ? gSavedSettings.getF32("ASStarsDensity") : 1.f;
    const F32 color_amount = on ? gSavedSettings.getF32("ASStarsColorAmount") : 0.f;
    const F32 saturation = on ? gSavedSettings.getF32("ASStarsSaturation") : 1.f;
    const F32 temperature_bias = on ? gSavedSettings.getF32("ASStarsTemperatureBias") : 0.f;
    const F32 brightness_variation = on ? gSavedSettings.getF32("ASStarsBrightnessVariation") : 0.f;
    const F32 size = on ? gSavedSettings.getF32("ASStarsSize") : 1.f;
    const F32 milky_way = on ? gSavedSettings.getF32("ASStarsMilkyWay") : 0.f;
    const S32 seed = on ? gSavedSettings.getS32("ASStarsSeed") : 0;

    const S32 mode = on ? gSavedSettings.getS32("ASStarsMode") : 0;
    const F32 magnitude_contrast = gSavedSettings.getF32("ASStarsMagnitudeContrast");

    sRealSky = mode == 1 && loadCatalogue();
    // Procedural with zero brightness contrast keeps the stock path: alpha
    // ignored by the shader, all stars equally bright.
    const F32 variation = llclamp(brightness_variation, 0.f, 1.f);
    sEncoded = sRealSky || variation > 0.f;

    // Count. Procedural: the Milky Way band adds stars on top of the
    // background field instead of taking them from it, so the rest of the
    // sky keeps its density. Real sky: the brightest stars of the catalogue.
    const U32 stars_per_density = sRealSky ? REAL_SKY_STARS_PER_DENSITY : STOCK_VISIBLE_STARS;
    const U32 background_count = (U32)llclamp(ll_round(stars_per_density * llmax(density, 0.f)), 0, (S32)MAX_STARS);
    U32 band_count = 0;
    if (sRealSky)
    {
        sCount = llmin(background_count, (U32)sCatalogue.size());
    }
    else
    {
        band_count = (U32)ll_round(background_count * llclamp(milky_way, 0.f, 1.f) * MILKY_WAY_EXTRA_STARS);
        sCount = llmin(background_count + band_count, MAX_STARS);
    }
    positions.resize(sCount);
    colors.resize(sCount);
    intensities.resize(sCount);
    sSizes.resize(sCount);

    // Seed 0 keeps stock behaviour: a different sky every time.
    std::mt19937 rng(seed > 0 ? (U32)seed : (U32)ll_rand());
    std::uniform_real_distribution<F32> uni(0.f, 1.f);

    const F32 amount = llclamp(color_amount, 0.f, 1.f);
    const F32 temperature_scale = std::pow(2.f, llclamp(temperature_bias, -1.f, 1.f));
    const F32 contrast = llclamp(magnitude_contrast, 0.05f, 1.f);
    // Real sky: magnitude of the faintest drawn star (the eye's limit).
    const F32 limit_mag = sRealSky && sCount > 0 ? sCatalogue[sCount - 1].mMag : 0.f;

    for (U32 i = 0; i < sCount; ++i)
    {
        F32 size_scale = 1.f;
        F32 temperature = 0.f;
        if (sRealSky)
        {
            const CatalogueStar& star = sCatalogue[i];
            positions[i] = star.mDir * radius;

            // Brightness relative to the stock star level: flux (relative
            // to the faintest drawn star) ^ contrast. Up to the stock level
            // it is log-encoded in vertex alpha; the rest grows the sprite.
            const F32 flux = std::pow(10.f, -0.4f * (star.mMag - limit_mag));
            const F32 level = std::pow(flux, contrast) * REAL_SKY_FAINT_LEVEL;
            intensities[i] = llclamp(1.f + std::log2(llmin(level, 1.f)) / REAL_SKY_LOG_RANGE, 0.f, 1.f);
            size_scale = llmin(std::sqrt(llmax(level, 1.f)), REAL_SKY_MAX_SIZE_SCALE);
            // Unknown B-V: a Sun-like color.
            temperature = star.mHasBV ? bvToTemperature(star.mBV) : 5800.f;
        }
        else
        {
            // Position: stock upper-hemisphere distribution for the
            // background, then the extra band stars.
            LLVector3 dir;
            if (i >= background_count)
            {
                dir = sampleBandDirection(rng);
            }
            else
            {
                dir.set(uni(rng) - 0.5f, uni(rng) - 0.5f, uni(rng) / 2.f);
                dir.normVec();
            }
            positions[i] = dir * radius;

            // Stock intensity distribution, in [0.1, 1].
            const F32 u = uni(rng);
            intensities[i] = llmin(std::pow(u, 2.f) + 0.1f, 1.f);
            if (sEncoded)
            {
                // Brightness contrast, modelled on the real sky (past the
                // stock x32 gain everything above ~1/32 of the stock level
                // is plain white, so contrast must come from levels below
                // it and from sprite size). Target: a synthetic magnitude
                // from the star-count law N(<m) ~ 10^(0.45 m), i.e. flux
                // ~ (1-u)^(-0.4/0.45) above the faintest star, which sits at
                // the real-sky faint level (median ~2x it, ~2% of stars
                // above the stock level). Blended in log space from the
                // stock level (contrast 0) to the target (contrast 1).
                const F32 target = std::pow(llmax(1.f - u, 1.e-4f), -0.4f / 0.45f) * REAL_SKY_FAINT_LEVEL;
                const F32 level = std::exp2(variation * std::log2(target));
                intensities[i] = llclamp(1.f + std::log2(llmin(level, 1.f)) / REAL_SKY_LOG_RANGE, 0.f, 1.f);
                size_scale = llmin(std::sqrt(llmax(level, 1.f)), REAL_SKY_MAX_SIZE_SCALE);
            }
        }

        // Stock greenish-white tint, blended toward a blackbody color.
        LLColor3 col(0.75f + uni(rng) * 0.25f, 1.f, 0.75f + uni(rng) * 0.25f);
        if (amount > 0.f)
        {
            if (!sRealSky)
            {
                temperature = sampleTemperature(rng);
            }
            const LLColor3 real = blackbody(temperature * temperature_scale);
            col = col * (1.f - amount) + real * amount;
        }
        const F32 lum = col.mV[VRED] * 0.2126f + col.mV[VGREEN] * 0.7152f + col.mV[VBLUE] * 0.0722f;
        const LLColor3 grey(lum, lum, lum);
        col = grey + (col - grey) * llmax(saturation, 0.f);
        // Encoded: alpha carries the brightness from the start (stock
        // starts at 1 and lets updateStarColors() settle it).
        colors[i].set(col, sEncoded ? intensities[i] : 1.f);
        colors[i].clamp();

        // Procedural: random stock size, blended toward the brightness-driven
        // size as brightness contrast rises.
        F32 base_size = REAL_SKY_BASE_SIZE * size_scale;
        if (!sRealSky)
        {
            const F32 stock_size = 16.f + uni(rng) * 20.f;
            base_size = stock_size + (base_size - stock_size) * variation;
        }
        sSizes[i] = base_size * llmax(size, 0.f);
    }
}

U32 ASStars::starCount()
{
    return sCount;
}

F32 ASStars::starSize(U32 i)
{
    return i < sSizes.size() ? sSizes[i] : 16.f;
}

F32 ASStars::brightness()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "ASStarsEnabled", true);
    static LLCachedControl<F32> brightness(gSavedSettings, "ASStarsBrightness", 1.f);
    return enabled ? llmax((F32)brightness, 0.f) : 1.f;
}

F32 ASStars::twinkleAmount()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "ASStarsEnabled", true);
    static LLCachedControl<F32> twinkle(gSavedSettings, "ASStarsTwinkle", 1.f);
    return enabled ? llclamp((F32)twinkle, 0.f, 1.f) : 1.f;
}

bool ASStars::realSkyActive()
{
    return sRealSky;
}

bool ASStars::encodedBrightness()
{
    return sEncoded;
}

void ASStars::applySkyTransform(F32 star_time, LLGLSLShader& shader)
{
    static LLStaticHashedString sStarUp("as_star_up");
    static LLStaticHashedString sHorizonFade("as_star_horizon_fade");
    static LLStaticHashedString sLogRange("as_star_log_range");

    // Encoded stars decode per-star brightness from vertex alpha; stock
    // ignores it.
    shader.uniform1f(sLogRange, sEncoded ? REAL_SKY_LOG_RANGE : 0.f);

    if (!sRealSky)
    {
        // Stock: slow spin around the zenith, no horizon fade (stars are
        // generated on the upper hemisphere only).
        gGL.rotatef(star_time * 0.01f, 0.f, 0.f, 1.f);
        shader.uniform1f(sHorizonFade, 0.f);
        return;
    }

    F32 tilt_deg, spin_deg;
    realSkyAngles(star_time, tilt_deg, spin_deg);

    // Applied right to left: spin first, then tilt.
    gGL.rotatef(tilt_deg, 1.f, 0.f, 0.f);
    gGL.rotatef(spin_deg, 0.f, 0.f, 1.f);

    // Local zenith expressed in the star frame (inverse rotation of +z), for
    // the vertex shader's below-horizon fade.
    const F32 tilt = tilt_deg * DEG_TO_RAD;
    const F32 spin = spin_deg * DEG_TO_RAD;
    shader.uniform3f(sStarUp, std::sin(tilt) * std::sin(spin), std::sin(tilt) * std::cos(spin), std::cos(tilt));
    shader.uniform1f(sHorizonFade, 1.f);
}

void ASStars::localToEquatorial(F32 star_time, F32 out[9])
{
    // local = A * equatorial with A = Rx(tilt) * Rz(spin) (the matrix
    // applySkyTransform() builds), so equatorial = A^T * local. A^T in GL
    // column-major order is A in row-major order.
    F32 tilt_deg, spin_deg;
    realSkyAngles(star_time, tilt_deg, spin_deg);
    const F32 ct = std::cos(tilt_deg * DEG_TO_RAD), st = std::sin(tilt_deg * DEG_TO_RAD);
    const F32 cs = std::cos(spin_deg * DEG_TO_RAD), ss = std::sin(spin_deg * DEG_TO_RAD);
    const F32 a[9] = { cs,      -ss,      0.f,
                       ct * ss,  ct * cs, -st,
                       st * ss,  st * cs,  ct };
    std::copy(a, a + 9, out);
}
