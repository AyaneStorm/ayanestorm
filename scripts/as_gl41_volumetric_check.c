/**
 * @file as_gl41_volumetric_check.c
 * @brief AyaneStorm: headless macOS OpenGL 4.1 core capability check for the
 *        exact volumetric lighting mode
 *        (doc/volumetric_lighting_exact_redesign_research.md).
 * @author chanayane@firestorm
 *
 * Build and run on macOS (no window, no dependencies):
 *   clang -DGL_SILENCE_DEPRECATION -framework OpenGL \
 *         scripts/as_gl41_volumetric_check.c -o /tmp/as_gl41_check && /tmp/as_gl41_check
 *
 * Exit code 0 when every required test passes.
 */

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

static void report(const char* name, int pass, const char* detail)
{
    printf("[%s] %s%s%s\n", pass ? "PASS" : "FAIL", name,
           detail ? " - " : "", detail ? detail : "");
    if (!pass)
    {
        ++g_failures;
    }
}

static int check_gl(const char* where)
{
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        printf("  GL error 0x%04X at %s\n", err, where);
        return 0;
    }
    return 1;
}

static GLuint compile(GLenum stage, const char* src)
{
    GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("  shader compile error:\n%s\n", log);
        exit(2);
    }
    return s;
}

static GLuint program(const char* vs, const char* fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        printf("  program link error:\n%s\n", log);
        exit(2);
    }
    return p;
}

// Fullscreen triangle from gl_VertexID (attribute-less draw, VAO bound).
static const char* VS_FULLSCREEN =
    "#version 410 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* FS_VALUE =
    "#version 410 core\n"
    "uniform float u_value;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(u_value, 0.0, 0.0, 1.0); }\n";

static const char* FS_UINT =
    "#version 410 core\n"
    "uniform uint u_count;\n"
    "out uvec4 frag_color;\n"
    "void main() { frag_color = uvec4(u_count); }\n";

// Vertex-shader raw depth fetch through a non-comparing sampler object.
static const char* VS_DEPTH_FETCH =
    "#version 410 core\n"
    "uniform sampler2D u_depth;\n"
    "uniform ivec2 u_texel;\n"
    "flat out float v_depth;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_depth = texelFetch(u_depth, u_texel, 0).r;\n"
    "}\n";

static const char* FS_DEPTH_FETCH =
    "#version 410 core\n"
    "flat in float v_depth;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(v_depth, 0.0, 0.0, 1.0); }\n";

static const char* FS_DEPTH_FETCH_FRAG =
    "#version 410 core\n"
    "uniform sampler2D u_depth;\n"
    "uniform ivec2 u_texel;\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(texelFetch(u_depth, u_texel, 0).r, 0.0, 0.0, 1.0); }\n";

// 4x4 depth texture. render_upload: written by rasterizing as a depth
// attachment (like a real shadow map), texel (2,3) gets depth 0.8; otherwise
// uploaded with glTexImage2D, texel (2,3) = 0.1 + 0.05 * 14 = 0.8.
static GLuint make_depth_texture(GLenum internal_format, int render_upload, float* expected)
{
    float depth[16];
    for (int i = 0; i < 16; ++i)
    {
        depth[i] = 0.1f + 0.05f * (float)i;
    }
    GLuint dtex;
    glGenTextures(1, &dtex);
    glBindTexture(GL_TEXTURE_2D, dtex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, 4, 4, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, render_upload ? NULL : depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    *expected = 0.8f;
    if (render_upload)
    {
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glViewport(0, 0, 4, 4);
        glDisable(GL_BLEND);
        glClearDepth(0.8);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    check_gl("make_depth_texture");
    return dtex;
}

static GLuint make_target(GLenum internal_format, GLenum format, GLenum type,
                          GLuint* tex_out, int* complete)
{
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, 1, 1, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    *complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    *tex_out = tex;
    glViewport(0, 0, 1, 1);
    return fbo;
}

static float read_red(void)
{
    float v = 0.f;
    glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &v);
    return v;
}

static void clear_zero(void)
{
    glDisable(GL_BLEND);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void additive_blend(void)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
}

static void draw_value(GLuint prog, GLint loc, float value)
{
    glUniform1f(loc, value);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Polygonal light volume accumulation pattern: per lit interval,
// -g(t_enter) then +g(t_exit), g(t) = (1 - exp(-sigma t)) / sigma.
// Returns |GPU - double reference|.
static double light_volume_sum_error(GLuint prog, GLint loc, double* expected_out,
                                     float* got_out)
{
    const double sigma = 0.002;
    double expected = 0.0;
    clear_zero();
    additive_blend();
    for (int i = 0; i < 64; ++i)
    {
        double t_enter = 3.7 + i * 7.9;
        double t_exit = t_enter + 2.3 + 0.37 * (i % 5);
        double g_enter = (1.0 - exp(-sigma * t_enter)) / sigma;
        double g_exit = (1.0 - exp(-sigma * t_exit)) / sigma;
        draw_value(prog, loc, (float)-g_enter);
        draw_value(prog, loc, (float)g_exit);
        expected += (double)(float)g_exit - (double)(float)g_enter;
    }
    float got = read_red();
    *expected_out = expected;
    *got_out = got;
    return fabs((double)got - expected);
}

int main(void)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix)
    {
        printf("[FAIL] no accelerated OpenGL 4.1 core pixel format\n");
        return 1;
    }
    CGLContextObj ctx = NULL;
    if (CGLCreateContext(pix, NULL, &ctx) != kCGLNoError || !ctx)
    {
        printf("[FAIL] CGLCreateContext\n");
        return 1;
    }
    CGLSetCurrentContext(ctx);

    printf("GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));
    printf("GLSL:        %s\n\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    GLuint prog_value = program(VS_FULLSCREEN, FS_VALUE);
    GLint loc_value = glGetUniformLocation(prog_value, "u_value");
    char detail[256];

    // 1. R32F render target, additive blending, negative values.
    {
        GLuint tex;
        int complete;
        make_target(GL_R32F, GL_RED, GL_FLOAT, &tex, &complete);
        report("R32F framebuffer complete", complete, NULL);
        if (complete)
        {
            glUseProgram(prog_value);
            clear_zero();
            additive_blend();
            draw_value(prog_value, loc_value, 1.5f);
            draw_value(prog_value, loc_value, 1.5f);
            draw_value(prog_value, loc_value, -0.25f);
            float v = read_red();
            snprintf(detail, sizeof(detail), "1.5 + 1.5 - 0.25 = %.6f (expected 2.75)", v);
            report("R32F additive blend (incl. negative)", fabsf(v - 2.75f) < 1e-6f &&
                   check_gl("R32F blend"), detail);

            double expected;
            float got;
            double err = light_volume_sum_error(prog_value, loc_value, &expected, &got);
            snprintf(detail, sizeof(detail), "got %.6f expected %.6f error %.6f (limit 0.01)",
                     got, expected, err);
            report("R32F light-volume cancellation precision", err < 0.01, detail);
        }
    }

    // 2. Informative: same cancellation in R16F (shows why R32F is needed).
    {
        GLuint tex;
        int complete;
        make_target(GL_R16F, GL_RED, GL_HALF_FLOAT, &tex, &complete);
        if (complete)
        {
            double expected;
            float got;
            glUseProgram(prog_value);
            double err = light_volume_sum_error(prog_value, loc_value, &expected, &got);
            printf("[INFO] R16F light-volume cancellation error %.6f (for comparison only)\n", err);
        }
        check_gl("R16F");
    }

    // 3. GPU-written instance count: fragment pass -> pixel pack buffer ->
    //    same buffer as GL_DRAW_INDIRECT_BUFFER -> glDrawArraysIndirect.
    {
        GLuint count_tex;
        int complete;
        GLuint count_fbo = make_target(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT,
                                       &count_tex, &complete);
        report("R32UI framebuffer complete", complete, NULL);

        GLuint prog_uint = program(VS_FULLSCREEN, FS_UINT);
        glUseProgram(prog_uint);
        glUniform1ui(glGetUniformLocation(prog_uint, "u_count"), 7u);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // DrawArraysIndirectCommand { count, instanceCount, first, reserved }
        GLuint cmd[4] = { 3u, 0u, 0u, 0u };
        GLuint buf;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buf);
        glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(cmd), cmd, GL_DYNAMIC_COPY);
        glBindFramebuffer(GL_FRAMEBUFFER, count_fbo);
        glReadPixels(0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, (void*)sizeof(GLuint));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        check_gl("pack instance count");

        GLuint target_tex;
        make_target(GL_R32F, GL_RED, GL_FLOAT, &target_tex, &complete);
        glUseProgram(prog_value);
        glUniform1f(loc_value, 1.0f);
        clear_zero();
        additive_blend();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buf);
        glDrawArraysIndirect(GL_TRIANGLES, (const void*)0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        int ok = check_gl("glDrawArraysIndirect");
        float v = read_red();
        snprintf(detail, sizeof(detail), "accumulated %.1f (expected 7.0 = 7 instances)", v);
        report("GPU-written indirect draw count", ok && fabsf(v - 7.f) < 1e-6f, detail);
    }

    // 4. Raw depth fetch (how SL shadow maps would be read by the light
    //    volume mesh). Required: at least one vertex-stage variant passes.
    //    The others isolate the cause if the sampler-object path fails.
    {
        GLuint prog_vs = program(VS_DEPTH_FETCH, FS_DEPTH_FETCH);
        GLuint prog_fs = program(VS_FULLSCREEN, FS_DEPTH_FETCH_FRAG);
        int any_vertex_pass = 0;
        // { internal format, upload via depth-attachment render, texture compare, sampler object, vertex stage }
        struct Variant { const char* name; GLenum fmt; int render_upload; int tex_compare; int use_sampler; int vertex; };
        const struct Variant variants[] = {
            { "VS D32F upload,  tex compare ON,  sampler NONE", GL_DEPTH_COMPONENT32F, 0, 1, 1, 1 },
            { "VS D32F upload,  tex compare OFF, no sampler  ", GL_DEPTH_COMPONENT32F, 0, 0, 0, 1 },
            { "VS D24 upload,   tex compare OFF, no sampler  ", GL_DEPTH_COMPONENT24,  0, 0, 0, 1 },
            { "VS D32F render,  tex compare ON,  sampler NONE", GL_DEPTH_COMPONENT32F, 1, 1, 1, 1 },
            { "VS D32F render,  tex compare OFF, no sampler  ", GL_DEPTH_COMPONENT32F, 1, 0, 0, 1 },
            { "FS D32F upload,  tex compare ON,  sampler NONE", GL_DEPTH_COMPONENT32F, 0, 1, 1, 0 },
            { "FS D32F upload,  tex compare OFF, no sampler  ", GL_DEPTH_COMPONENT32F, 0, 0, 0, 0 },
            { "FS D32F render,  tex compare ON,  sampler NONE", GL_DEPTH_COMPONENT32F, 1, 1, 1, 0 },
        };
        for (size_t k = 0; k < sizeof(variants) / sizeof(variants[0]); ++k)
        {
            const struct Variant* var = &variants[k];
            float expected = 0.f;
            GLuint dtex = make_depth_texture(var->fmt, var->render_upload, &expected);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, dtex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
                            var->tex_compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

            GLuint sampler = 0;
            if (var->use_sampler)
            {
                glGenSamplers(1, &sampler);
                glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
                glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindSampler(0, sampler);
            }

            // make_target binds its own texture on the active unit: rebind
            // the depth texture to unit 0 afterwards.
            GLuint tex;
            int complete;
            make_target(GL_R32F, GL_RED, GL_FLOAT, &tex, &complete);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, dtex);
            GLuint prog = var->vertex ? prog_vs : prog_fs;
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "u_depth"), 0);
            glUniform2i(glGetUniformLocation(prog, "u_texel"), 2, 3);
            clear_zero();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            int ok = check_gl(var->name);
            float v = read_red();
            int pass = ok && fabsf(v - expected) < 1e-4f;
            printf("[%s] %s - fetched %.6f (expected %.6f)\n",
                   pass ? "ok  " : "bad ", var->name, v, expected);
            if (pass && var->vertex)
            {
                any_vertex_pass = 1;
            }
            glBindSampler(0, 0);
            if (sampler)
            {
                glDeleteSamplers(1, &sampler);
            }
        }
        report("vertex-stage raw depth fetch (any variant)", any_vertex_pass, NULL);
    }

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL REQUIRED TESTS PASSED",
           g_failures, g_failures == 1 ? "" : "s");

    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pix);
    return g_failures ? 1 : 0;
}
