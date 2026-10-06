/**
 * @file asstars.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local star catalogue: density, blackbody colors, size and
 * twinkle. Defaults reproduce the stock LLVOWLSky stars.
 */

#include "llviewerprecompiledheaders.h"

#include "asstars.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "llcontrol.h"
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

    // Settings that change the catalogue and therefore need a rebuild.
    const std::vector<std::string> sCatalogueControls = {
        "ASStarsDensity", "ASStarsColorAmount", "ASStarsSaturation",
        "ASStarsTemperatureBias", "ASStarsBrightnessSpread", "ASStarsSize",
        "ASStarsMilkyWay", "ASStarsSeed", "ASStarsEnabled"
    };
    // Every resettable setting of the panel.
    const std::vector<std::string> sAllControls = {
        "ASStarsDensity", "ASStarsColorAmount", "ASStarsSaturation",
        "ASStarsTemperatureBias", "ASStarsBrightness", "ASStarsBrightnessSpread",
        "ASStarsSize", "ASStarsTwinkle", "ASStarsMilkyWay", "ASStarsSeed"
    };

    U32 sCount = 0;
    std::vector<F32> sSizes;

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
}

void ASStars::generate(F32 radius, std::vector<LLVector3>& positions,
                       std::vector<LLColor4>& colors, std::vector<F32>& intensities)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "ASStarsEnabled", true);
    static LLCachedControl<F32> density_ctl(gSavedSettings, "ASStarsDensity", 1.f);
    static LLCachedControl<F32> color_amount_ctl(gSavedSettings, "ASStarsColorAmount", 0.f);
    static LLCachedControl<F32> saturation_ctl(gSavedSettings, "ASStarsSaturation", 1.f);
    static LLCachedControl<F32> temperature_bias_ctl(gSavedSettings, "ASStarsTemperatureBias", 0.f);
    static LLCachedControl<F32> brightness_spread_ctl(gSavedSettings, "ASStarsBrightnessSpread", 2.f);
    static LLCachedControl<F32> size_ctl(gSavedSettings, "ASStarsSize", 1.f);
    static LLCachedControl<F32> milky_way_ctl(gSavedSettings, "ASStarsMilkyWay", 0.f);
    static LLCachedControl<S32> seed_ctl(gSavedSettings, "ASStarsSeed", 0);

    // Master toggle off: the stock parameters (setting defaults), whatever
    // the panel holds.
    const bool on = enabled;
    const F32 density = on ? (F32)density_ctl : 1.f;
    const F32 color_amount = on ? (F32)color_amount_ctl : 0.f;
    const F32 saturation = on ? (F32)saturation_ctl : 1.f;
    const F32 temperature_bias = on ? (F32)temperature_bias_ctl : 0.f;
    const F32 brightness_spread = on ? (F32)brightness_spread_ctl : 2.f;
    const F32 size = on ? (F32)size_ctl : 1.f;
    const F32 milky_way = on ? (F32)milky_way_ctl : 0.f;
    const S32 seed = on ? (S32)seed_ctl : 0;

    // The Milky Way band adds stars on top of the background field instead
    // of taking them from it, so the rest of the sky keeps its density.
    const F32 band = llclamp((F32)milky_way, 0.f, 1.f);
    const U32 background_count = (U32)llclamp(ll_round(STOCK_VISIBLE_STARS * llmax((F32)density, 0.f)), 0, (S32)MAX_STARS);
    const U32 band_count = (U32)ll_round(background_count * band * MILKY_WAY_EXTRA_STARS);
    sCount = llmin(background_count + band_count, MAX_STARS);
    positions.resize(sCount);
    colors.resize(sCount);
    intensities.resize(sCount);
    sSizes.resize(sCount);

    // Seed 0 keeps stock behaviour: a different sky every time.
    std::mt19937 rng(seed > 0 ? (U32)(S32)seed : (U32)ll_rand());
    std::uniform_real_distribution<F32> uni(0.f, 1.f);

    const F32 amount = llclamp((F32)color_amount, 0.f, 1.f);
    const F32 spread = llmax((F32)brightness_spread, 0.01f);
    const F32 temperature_scale = std::pow(2.f, llclamp((F32)temperature_bias, -1.f, 1.f));

    for (U32 i = 0; i < sCount; ++i)
    {
        // Position: stock upper-hemisphere distribution for the background,
        // then the extra band stars.
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

        intensities[i] = llmin(std::pow(uni(rng), spread) + 0.1f, 1.f);

        // Stock greenish-white tint, blended toward a blackbody color.
        LLColor3 col(0.75f + uni(rng) * 0.25f, 1.f, 0.75f + uni(rng) * 0.25f);
        if (amount > 0.f)
        {
            const LLColor3 real = blackbody(sampleTemperature(rng) * temperature_scale);
            col = col * (1.f - amount) + real * amount;
        }
        const F32 lum = col.mV[VRED] * 0.2126f + col.mV[VGREEN] * 0.7152f + col.mV[VBLUE] * 0.0722f;
        const LLColor3 grey(lum, lum, lum);
        col = grey + (col - grey) * llmax((F32)saturation, 0.f);
        colors[i].set(col, 1.f);
        colors[i].clamp();

        sSizes[i] = (16.f + uni(rng) * 20.f) * llmax((F32)size, 0.f);
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
