/**
 * @file asMotionBlurF.glsl
 * @author chanayane@firestorm
 * @brief Screen-space camera motion blur via depth + previous-frame reprojection.
 */
uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform vec2 screen_res;

uniform mat4 inv_curr_proj;
uniform mat4 inv_curr_modelview;
uniform mat4 prev_modelview_proj;

uniform float motion_blur_strength;
uniform float motion_blur_max_length;
uniform int motion_blur_samples;
uniform int motion_blur_debug;

in vec2 vary_fragcoord;
out vec4 frag_color;

#define AS_MOTION_BLUR_MAX_SAMPLES 24

float asLinearDepth(float d, float znear, float zfar)
{
    d = d * 2.0 - 1.0;
    return znear * 2.0 * zfar / (zfar + znear - d * (zfar - znear));
}

// Reconstructs the current frame's world-space position for a screen UV + device depth.
vec3 asWorldPositionFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view_pos = inv_curr_proj * ndc;
    view_pos /= view_pos.w;
    vec4 world_pos = inv_curr_modelview * view_pos;
    return world_pos.xyz;
}

void main()
{
    vec2 uv = vary_fragcoord;

    // <AS:Chanayane> Matrix-sanity probe: bypasses depth reconstruction
    // entirely. Reprojects the current pixel's own clip-space position
    // (at mid-depth) through inv_curr_proj/inv_curr_modelview and back
    // through prev_modelview_proj, then visualizes the raw UV delta the
    // same way as mode 2. If this is also flat/zero, the captured
    // matrices themselves are identical or identity - not a depth or
    // per-pixel reconstruction issue.
    if (motion_blur_debug == 3)
    {
        vec4 ndc = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
        vec4 view_pos = inv_curr_proj * ndc;
        view_pos /= view_pos.w;
        vec4 world_pos4 = inv_curr_modelview * view_pos;
        vec4 reproj = prev_modelview_proj * world_pos4;
        vec2 reproj_uv = uv;
        if (reproj.w > 0.0)
        {
            reproj_uv = (reproj.xy / reproj.w) * 0.5 + 0.5;
        }
        vec2 delta = uv - reproj_uv;
        frag_color = vec4(delta * 2000.0 + 0.5, reproj.w > 0.0 ? 0.0 : 1.0, 1.0);
        return;
    }
    // </AS:Chanayane>

    float center_depth = texture(depthMap, uv).r;

    // Depth of 1.0 is the far clip / sky - reprojecting the sky produces no
    // useful parallax and can blow up numerically, so leave it untouched.
    if (center_depth >= 1.0)
    {
        frag_color = texture(diffuseRect, uv);
        return;
    }

    vec3 world_pos = asWorldPositionFromDepth(uv, center_depth);

    vec4 prev_clip = prev_modelview_proj * vec4(world_pos, 1.0);
    if (prev_clip.w <= 0.0)
    {
        if (motion_blur_debug == 1)
        {
            frag_color = vec4(1.0, 0.0, 1.0, 1.0); // magenta: behind previous-frame camera
            return;
        }
        frag_color = texture(diffuseRect, uv);
        return;
    }
    vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;

    // Raw, unscaled reprojection velocity - motion_blur_strength is applied
    // later, only as a sampling-reach multiplier, so the near-zero-motion
    // gate below always means "is the camera actually moving," regardless
    // of the Strength setting.
    vec2 vel_pixels = (uv - prev_uv) * screen_res;
    float speed = length(vel_pixels);

    // <AS:Chanayane> Debug visualization - not tagged in .cpp since this whole
    // file is AyaneStorm-original, but the mode itself is a temporary
    // development aid pending removal once the effect is confirmed working.
    if (motion_blur_debug == 1)
    {
        // Velocity magnitude heatmap, boosted range: black = 0px, red = 4px.
        // (Not scaled by motion_blur_max_length - real per-frame camera motion
        // is usually a small fraction of a pixel to a few pixels, not tens.)
        frag_color = vec4(clamp(speed / 4.0, 0.0, 1.0), 0.0, 0.0, 1.0);
        return;
    }
    if (motion_blur_debug == 2)
    {
        // Raw UV delta as signed color: red channel = X delta, green = Y delta.
        // Scaled way up (x2000) so even sub-pixel per-frame deltas are visible.
        vec2 uv_delta = uv - prev_uv;
        frag_color = vec4(uv_delta * 2000.0 + 0.5, 0.0, 1.0);
        return;
    }
    // </AS:Chanayane>

    if (speed < 0.05)
    {
        frag_color = texture(diffuseRect, uv);
        return;
    }

    // Strength scales the sampling reach only, after the real-motion gate above.
    vel_pixels *= motion_blur_strength;
    speed *= motion_blur_strength;

    if (speed > motion_blur_max_length)
    {
        vel_pixels *= motion_blur_max_length / speed;
    }

    vec2 step_uv = (vel_pixels / screen_res) / float(max(motion_blur_samples - 1, 1));

    // View-space linear depth of the center pixel, used to reject samples that
    // belong to a different surface than the one being blurred (avoids
    // smearing a moving foreground silhouette into the background or vice versa).
    float center_linear_depth = asLinearDepth(center_depth, 0.1, 4096.0);
    float depth_reject_scale = max(center_linear_depth * 0.02, 0.05);

    vec3 accum = vec3(0.0);
    float total_weight = 0.0;
    float half_samples = float(motion_blur_samples - 1) * 0.5;

    // Fixed-iteration loop (no uniform-dependent break): samples beyond the
    // active quality tier are masked to zero weight instead of skipped.
    for (int i = 0; i < AS_MOTION_BLUR_MAX_SAMPLES; ++i)
    {
        float sample_active = (i < motion_blur_samples) ? 1.0 : 0.0;

        vec2 sample_uv = uv + step_uv * (float(i) - half_samples);
        sample_uv = clamp(sample_uv, 0.5 / screen_res, vec2(1.0) - 0.5 / screen_res);

        float sample_depth = texture(depthMap, sample_uv).r;
        float sample_linear_depth = asLinearDepth(sample_depth, 0.1, 4096.0);

        float depth_delta = abs(sample_linear_depth - center_linear_depth);
        float weight = (1.0 - abs(float(i) - half_samples) / max(half_samples, 0.0001));
        weight *= 1.0 - smoothstep(depth_reject_scale, depth_reject_scale * 4.0, depth_delta);
        weight *= sample_active;

        accum += texture(diffuseRect, sample_uv).rgb * weight;
        total_weight += weight;
    }

    if (total_weight <= 0.0001)
    {
        frag_color = texture(diffuseRect, uv);
        return;
    }

    frag_color = vec4(accum / total_weight, 1.0);
}
