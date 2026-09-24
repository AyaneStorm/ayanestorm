/**
 * @file asdofaperture.cpp
 * @author chanayane@firestorm
 * @brief Lens-position samples for aperture-sampled depth of field.
 */
#include "llviewerprecompiledheaders.h"
#include "asdofaperture.h"

#include <cmath>

#include "llmath.h"
#include "llviewercontrol.h"

namespace
{
    // R4 Kronecker sequence: root of x^5 = x + 1. Dimensions 0/1 drive the
    // lens, 2/3 the pixel jitter; one nested index for both.
    constexpr F64 R4_ROOT = 1.16730397826141868426;
    constexpr F64 PI_D = 3.14159265358979323846;

    F64 r4(U32 index, S32 dimension)
    {
        return std::fmod(0.5 + index / std::pow(R4_ROOT, dimension + 1), 1.0);
    }

    // Unit-circumradius edge radius at a polar angle (rotation excluded).
    F64 boundary(F64 angle, S32 blades, F64 roundness)
    {
        if (blades < 3 || roundness >= 1.0)
        {
            return 1.0;
        }
        const F64 half = PI_D / blades;
        const F64 local = std::fmod(angle, 2.0 * half) - half;
        return (1.0 - roundness) * std::cos(half) / std::cos(local) + roundness;
    }

    // Area within one blade from -pi/n to x: integral of boundary^2 / 2.
    // Uniform area density needs the angle marginal proportional to boundary^2.
    F64 bladeCdf(F64 x, S32 blades, F64 roundness)
    {
        const F64 half = PI_D / blades;
        const F64 a = (1.0 - roundness) * std::cos(half);
        const F64 b = roundness;
        auto antiderivative = [a, b](F64 t)
        {
            return (a * a * std::tan(t) + 2.0 * a * b * std::log(1.0 / std::cos(t) + std::tan(t))
                    + b * b * t) * 0.5;
        };
        return antiderivative(x) - antiderivative(-half);
    }
}

namespace ASDoFAperture
{
    Shape shapeFromSettings()
    {
        Shape shape;
        shape.mBlades = llclamp(gSavedSettings.getS32("ASDepthOfFieldApertureBlades"), 0, 12);
        shape.mRoundness = llclamp(gSavedSettings.getF32("ASDepthOfFieldApertureRoundness"), 0.f, 1.f);
        shape.mRotation = gSavedSettings.getF32("ASDepthOfFieldApertureRotation") * DEG_TO_RAD;
        shape.mAnamorphic = llclamp(gSavedSettings.getF32("ASDepthOfFieldAnamorphicRatio"), 0.1f, 2.f);
        return shape;
    }

    void generate(const Shape& shape, U32 count, std::vector<glm::vec2>& lens,
                  std::vector<glm::vec2>& pixel_jitter)
    {
        lens.clear();
        lens.reserve(count);
        pixel_jitter.clear();
        pixel_jitter.reserve(count);

        const F64 roundness = shape.mRoundness;
        const bool polygon = shape.mBlades >= 3 && roundness < 1.0;
        const F64 half = polygon ? PI_D / shape.mBlades : 0.0;
        const F64 blade_area = polygon ? bladeCdf(half, shape.mBlades, roundness) : 0.0;

        for (U32 i = 0; i < count; ++i)
        {
            const F64 u = r4(i, 0);
            const F64 v = r4(i, 1);
            pixel_jitter.emplace_back((F32)(r4(i, 2) - 0.5), (F32)(r4(i, 3) - 0.5));

            F64 angle;
            if (polygon)
            {
                // Pick the blade, then invert its area CDF by bisection
                // (monotonic; 60 steps reach double precision).
                const F64 blade_f = u * shape.mBlades;
                const F64 blade = std::floor(blade_f);
                const F64 target = (blade_f - blade) * blade_area;
                F64 lo = -half;
                F64 hi = half;
                for (S32 step = 0; step < 60; ++step)
                {
                    const F64 mid = 0.5 * (lo + hi);
                    if (bladeCdf(mid, shape.mBlades, roundness) < target)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                // Local 0 is the blade centre, at polar angle pi/n.
                angle = 0.5 * (lo + hi) + half + blade * 2.0 * half;
            }
            else
            {
                angle = 2.0 * PI_D * u;
            }

            const F64 r = std::sqrt(v) * boundary(angle, shape.mBlades, roundness);
            angle += shape.mRotation;
            lens.emplace_back((F32)(shape.mAnamorphic * r * std::cos(angle)),
                              (F32)(r * std::sin(angle)));
        }
    }

    F32 spectralCoordinate(U32 index)
    {
        // Van der Corput (bit reversal), rotated by 1/2 so sample 0 is green
        // (s = 0); every power-of-two prefix is an even grid over [-1, 1).
        U32 bits = index;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
        bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
        const F64 u = std::fmod((F64)bits / 4294967296.0 + 0.5, 1.0);
        return (F32)(2.0 * u - 1.0);
    }

    glm::vec3 spectralWeights(F32 s)
    {
        return glm::vec3(1.f + s, 1.5f * (1.f - s * s), 1.f - s);
    }
}
