/**
 * @file asdofcamera.cpp
 * @author chanayane@firestorm
 * @brief Thin-lens camera math for aperture-sampled depth of field.
 */
#include "llviewerprecompiledheaders.h"
#include "asdofcamera.h"

#include <cmath>

#include "llmath.h"

namespace ASDoFCamera
{
    Lens makeLens(F32 fov_y, F32 default_fov_y, F32 default_focal_length_mm,
                  F32 f_number, F32 focus_distance)
    {
        Lens lens;
        if (!(fov_y > 0.f && fov_y < F_PI && default_fov_y > 0.f && default_fov_y < F_PI
              && default_focal_length_mm > 0.f && f_number > 0.f && std::isfinite(focus_distance)))
        {
            return lens;
        }

        // Fixed sensor height from the default lens, as LLPipeline::renderDoF.
        const F32 sensor_height_mm = 2.f * default_focal_length_mm * tanf(default_fov_y * 0.5f);
        const F32 focal_length = sensor_height_mm / (2.f * tanf(fov_y * 0.5f)) / 1000.f;
        if (!(focus_distance > focal_length))
        {
            return lens;
        }

        lens.mFocusDistance = focus_distance;
        lens.mFocalLength = focal_length;
        lens.mFNumber = f_number;
        lens.mApertureRadius = focal_length / (2.f * f_number);
        lens.mFovY = fov_y;
        lens.mValid = true;
        return lens;
    }

    glm::vec2 lensOffset(const Lens& lens, const glm::vec2& unit_disk_sample)
    {
        return unit_disk_sample * lens.mApertureRadius;
    }

    glm::mat4 lensModelview(const glm::mat4& modelview, const glm::vec2& offset)
    {
        // T(-offset) * M: eye rows x/y each subtract offset times row w.
        glm::mat4 result = modelview;
        for (int column = 0; column < 4; ++column)
        {
            result[column][0] -= offset.x * modelview[column][3];
            result[column][1] -= offset.y * modelview[column][3];
        }
        return result;
    }

    glm::mat4 lensProjection(const glm::mat4& projection, const glm::vec2& offset,
                             F32 focus_distance)
    {
        llassert(projection[2][3] == -1.f && projection[3][3] == 0.f);
        llassert(focus_distance > 0.f);

        // clip.x = P00 * (x - dx) + P20 * z; at z == -focus the added
        // -P00 * dx / focus * z term cancels -P00 * dx. Same for y.
        glm::mat4 result = projection;
        result[2][0] -= projection[0][0] * offset.x / focus_distance;
        result[2][1] -= projection[1][1] * offset.y / focus_distance;
        return result;
    }

    glm::mat4 jitterProjection(const glm::mat4& projection, const glm::vec2& jitter_px,
                               F32 viewport_width, F32 viewport_height)
    {
        llassert(projection[2][3] == -1.f && viewport_width > 0.f && viewport_height > 0.f);

        // clip.x += 2 * jx / W * w with w == -z_eye.
        glm::mat4 result = projection;
        result[2][0] -= 2.f * jitter_px.x / viewport_width;
        result[2][1] -= 2.f * jitter_px.y / viewport_height;
        return result;
    }

    F32 cocRadiusPixels(const Lens& lens, F32 depth, F32 viewport_height_px)
    {
        if (!lens.mValid || !(depth > 0.f))
        {
            return 0.f;
        }
        return lens.mApertureRadius * fabsf(1.f / lens.mFocusDistance - 1.f / depth)
            * viewport_height_px / (2.f * tanf(lens.mFovY * 0.5f));
    }
}
