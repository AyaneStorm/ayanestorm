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
    constexpr F64 PI_D = 3.14159265358979323846;

    // Owen-scrambled Sobol sequence, 5 dimensions: 0/1 lens position, 2/3
    // pixel jitter, 4 axial-CA wavelength. Power-of-two prefixes are
    // (t, m, 2)-nets (lens pair t = 0, jitter pair t = 1): stratified in
    // every elementary interval, which suits thin strands as well as the
    // aperture. Kronecker sequences left lattice structure that the polar
    // aperture mapping turned into petal/spiral patterns in point-light
    // bokeh; hash-based Owen scrambling (Laine-Karras permutation on the
    // reversed bits) randomizes within strata instead, so the pattern
    // becomes fine grain without clumping. Nested and deterministic.
    // Mirrored by dof_reference.py (sobol_owen_bits).
    constexpr S32 SOBOL_DIMENSIONS = 5;
    constexpr U32 SOBOL_SEEDS[SOBOL_DIMENSIONS] = { 0x8e3ba9d1u, 0x2f9b1c4du, 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u };

    struct SobolDirections
    {
        U32 mV[SOBOL_DIMENSIONS][32];

        SobolDirections()
        {
            // Joe-Kuo primitive polynomials (degree s, coefficients a,
            // initial m) for dimensions 1..4; dimension 0 is van der Corput.
            struct Polynomial { S32 mS; U32 mA; U32 mM[3]; };
            static const Polynomial POLYNOMIALS[SOBOL_DIMENSIONS - 1] = {
                { 1, 0, { 1, 0, 0 } }, { 2, 1, { 1, 3, 0 } },
                { 3, 1, { 1, 3, 1 } }, { 3, 2, { 1, 1, 1 } } };
            for (S32 k = 0; k < 32; ++k)
            {
                mV[0][k] = 1u << (31 - k);
            }
            for (S32 d = 1; d < SOBOL_DIMENSIONS; ++d)
            {
                const Polynomial& p = POLYNOMIALS[d - 1];
                U32* v = mV[d];
                for (S32 k = 0; k < p.mS; ++k)
                {
                    v[k] = p.mM[k] << (31 - k);
                }
                for (S32 k = p.mS; k < 32; ++k)
                {
                    U32 x = v[k - p.mS] ^ (v[k - p.mS] >> p.mS);
                    for (S32 j = 1; j < p.mS; ++j)
                    {
                        if ((p.mA >> (p.mS - 1 - j)) & 1u)
                        {
                            x ^= v[k - j];
                        }
                    }
                    v[k] = x;
                }
            }
        }
    };

    U32 reverseBits(U32 x)
    {
        x = (x << 16) | (x >> 16);
        x = ((x & 0x00ff00ffu) << 8) | ((x & 0xff00ff00u) >> 8);
        x = ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
        x = ((x & 0x33333333u) << 2) | ((x & 0xccccccccu) >> 2);
        x = ((x & 0x55555555u) << 1) | ((x & 0xaaaaaaaau) >> 1);
        return x;
    }

    U32 laineKarras(U32 x, U32 seed)
    {
        x += seed;
        x ^= x * 0x6c50b47cu;
        x ^= x * 0xb82f1e52u;
        x ^= x * 0xc7afe638u;
        x ^= x * 0x8d22f6e6u;
        return x;
    }

    U32 sobolOwenBits(U32 index, S32 dimension)
    {
        static const SobolDirections directions;
        U32 x = 0;
        for (S32 k = 0; index; index >>= 1, ++k)
        {
            if (index & 1u)
            {
                x ^= directions.mV[dimension][k];
            }
        }
        return reverseBits(laineKarras(reverseBits(x), SOBOL_SEEDS[dimension]));
    }

    // [0, 1), exact in double.
    F64 sobolOwen(U32 index, S32 dimension)
    {
        return (F64)sobolOwenBits(index, dimension) / 4294967296.0;
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
            const F64 u = sobolOwen(i, 0);
            const F64 v = sobolOwen(i, 1);
            pixel_jitter.emplace_back((F32)(sobolOwen(i, 2) - 0.5), (F32)(sobolOwen(i, 3) - 0.5));

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
        // Its own Sobol dimension: a rescrambled van der Corput would be
        // dimension 0 (the lens angle) and tie colours to aperture sectors.
        return (F32)(2.0 * sobolOwen(index, 4) - 1.0);
    }

    glm::vec3 spectralWeights(F32 s)
    {
        return glm::vec3(1.f + s, 1.5f * (1.f - s * s), 1.f - s);
    }

    F32 pupilRadius2(U32 index)
    {
        // generate() places sample index at radius sqrt(v) * boundary.
        return (F32)sobolOwen(index, 1);
    }
}
