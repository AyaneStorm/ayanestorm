/**
 * @file asambientocclusion.cpp
 * @author chanayane@firestorm
 * @brief Optional XeGTAO-derived ambient-occlusion backend.
 *
 * Derived from the Intel XeGTAO implementation:
 * https://github.com/GameTechDev/XeGTAO (see LICENSE in that repository).
 */
#include "llviewerprecompiledheaders.h"

#include "asambientocclusion.h"

#include <array>
#include <utility>

#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

namespace
{
    constexpr U32 GTAO_MIP_COUNT = 5;
    constexpr U32 HILBERT_SIZE = 64;

    struct Resources
    {
        GLuint depth = 0;
        GLuint visibility[2] = { 0, 0 };
        GLuint edges = 0;
        GLuint hilbert = 0;
        GLuint fbo = 0;
        U32 width = 0;
        U32 height = 0;
        bool bent_normals = false;
        bool valid = false;
    };

    struct ImageBinding
    {
        GLint texture = 0;
        GLint level = 0;
        GLint layered = GL_FALSE;
        GLint layer = 0;
        GLint access = GL_READ_ONLY;
        GLint format = GL_R8;
    };

    Resources sResources;
    LLGLSLShader sDepthProgram;
    LLGLSLShader sDownsampleProgram;
    LLGLSLShader sMainMediumProgram;
    LLGLSLShader sMainHighProgram;
    LLGLSLShader sMainUltraProgram;
    LLGLSLShader sMainCinematicProgram;
    LLGLSLShader sDenoiseProgram;
    LLGLSLShader sBentMainMediumProgram;
    LLGLSLShader sBentMainHighProgram;
    LLGLSLShader sBentMainUltraProgram;
    LLGLSLShader sBentMainCinematicProgram;
    LLGLSLShader sBentDenoiseProgram;
    LLGLSLShader sComputeDepthProgram;
    LLGLSLShader sComputeMainMediumProgram;
    LLGLSLShader sComputeMainHighProgram;
    LLGLSLShader sComputeMainUltraProgram;
    LLGLSLShader sComputeMainCinematicProgram;
    LLGLSLShader sComputeDenoiseProgram;
    LLGLSLShader sComputeBentMainMediumProgram;
    LLGLSLShader sComputeBentMainHighProgram;
    LLGLSLShader sComputeBentMainUltraProgram;
    LLGLSLShader sComputeBentMainCinematicProgram;
    LLGLSLShader sComputeBentDenoiseProgram;
    bool sFragmentShadersLoaded = false;
    bool sComputeShadersLoaded = false;
    bool sFrameValid = false;
    bool sFrameHasBentNormals = false;
    U32 sFinalVisibility = 0;
    ASAmbientOcclusion::Technique sEffectiveTechnique = ASAmbientOcclusion::LEGACY_SSAO;
    ASAmbientOcclusion::Backend sEffectiveBackend = ASAmbientOcclusion::AUTO;
    S32 sBoundResultChannel = -1;
    bool sWarnedAllocation = false;
    bool sWarnedRender = false;
    bool sWarnedForcedCompute = false;

    const LLStaticHashedString U_SOURCE_DEPTH("source_depth");
    const LLStaticHashedString U_PROJECTION_DEPTH("gtao_projection_depth");
    const LLStaticHashedString U_ORTHOGRAPHIC("gtao_orthographic");
    const LLStaticHashedString U_SOURCE_MIP("gtao_source_mip");
    const LLStaticHashedString U_SOURCE_SIZE("gtao_source_size");
    const LLStaticHashedString U_RADIUS("gtao_radius");
    const LLStaticHashedString U_PIXEL_SIZE("gtao_pixel_size");
    const LLStaticHashedString U_NDC_MUL("gtao_ndc_to_view_mul");
    const LLStaticHashedString U_NDC_ADD("gtao_ndc_to_view_add");
    const LLStaticHashedString U_POWER("gtao_power");
    const LLStaticHashedString U_ORTHOGRAPHIC_VIEW("gtao_orthographic_view");
    const LLStaticHashedString U_VIEWPORT("gtao_viewport");
    const LLStaticHashedString U_FINAL_PASS("gtao_final_pass");
    const LLStaticHashedString U_BLUR_BETA("gtao_blur_beta");
    const LLStaticHashedString U_RESULT("gtao_visibility_map");

    void clearGLErrors()
    {
        while (glGetError() != GL_NO_ERROR) {}
    }

    U32 hilbertIndex(U32 x, U32 y)
    {
        U32 index = 0;
        for (U32 level = HILBERT_SIZE / 2; level > 0; level /= 2)
        {
            U32 region_x = (x & level) != 0;
            U32 region_y = (y & level) != 0;
            index += level * level * ((3 * region_x) ^ region_y);
            if (region_y == 0)
            {
                if (region_x == 1)
                {
                    x = HILBERT_SIZE - 1 - x;
                    y = HILBERT_SIZE - 1 - y;
                }
                std::swap(x, y);
            }
        }
        return index;
    }

    void destroyResources(Resources& resources)
    {
        if (resources.depth) glDeleteTextures(1, &resources.depth);
        if (resources.visibility[0] || resources.visibility[1])
        {
            glDeleteTextures(2, resources.visibility);
        }
        if (resources.edges) glDeleteTextures(1, &resources.edges);
        if (resources.hilbert) glDeleteTextures(1, &resources.hilbert);
        if (resources.fbo) glDeleteFramebuffers(1, &resources.fbo);
        resources = Resources();
    }

    void configureTexture(GLuint texture, GLint min_filter, GLint mag_filter)
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    bool allocateTexture2D(GLuint& texture, GLenum internal_format, U32 width, U32 height,
                           GLenum format, GLenum type)
    {
        glGenTextures(1, &texture);
        configureTexture(texture, GL_NEAREST, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, nullptr);
        return glGetError() == GL_NO_ERROR;
    }

    bool checkAttachment(GLuint fbo, GLenum attachment, GLuint texture, GLint level)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, level);
        GLenum draw_buffer = attachment;
        glDrawBuffers(1, &draw_buffer);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    bool allocateNewResources(Resources& next, U32 width, U32 height, bool bent_normals)
    {
        clearGLErrors();
        GLint saved_texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
        const GLuint saved_fbo = LLRenderTarget::sCurFBO;

        next.width = width;
        next.height = height;
        next.bent_normals = bent_normals;
        glGenTextures(1, &next.depth);
        configureTexture(next.depth, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GTAO_MIP_COUNT - 1);
        for (U32 mip = 0; mip < GTAO_MIP_COUNT; ++mip)
        {
            const U32 mip_width = llmax(1u, width >> mip);
            const U32 mip_height = llmax(1u, height >> mip);
            glTexImage2D(GL_TEXTURE_2D, mip, GL_R16F, mip_width, mip_height,
                         0, GL_RED, GL_FLOAT, nullptr);
        }

        bool success = glGetError() == GL_NO_ERROR;
        const GLenum ao_internal_format = bent_normals ? GL_RGBA8 : GL_R8;
        const GLenum ao_format = bent_normals ? GL_RGBA : GL_RED;
        success = allocateTexture2D(next.visibility[0], ao_internal_format, width, height,
                                    ao_format, GL_UNSIGNED_BYTE) && success;
        success = allocateTexture2D(next.visibility[1], ao_internal_format, width, height,
                                    ao_format, GL_UNSIGNED_BYTE) && success;
        success = allocateTexture2D(next.edges, GL_R8, width, height,
                                    GL_RED, GL_UNSIGNED_BYTE) && success;

        std::array<U16, HILBERT_SIZE * HILBERT_SIZE> hilbert;
        for (U32 y = 0; y < HILBERT_SIZE; ++y)
        {
            for (U32 x = 0; x < HILBERT_SIZE; ++x)
            {
                hilbert[x + y * HILBERT_SIZE] = (U16)hilbertIndex(x, y);
            }
        }
        glGenTextures(1, &next.hilbert);
        configureTexture(next.hilbert, GL_NEAREST, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, HILBERT_SIZE, HILBERT_SIZE,
                     0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, hilbert.data());
        success = (glGetError() == GL_NO_ERROR) && success;

        glGenFramebuffers(1, &next.fbo);
        for (U32 mip = 0; mip < GTAO_MIP_COUNT && success; ++mip)
        {
            success = checkAttachment(next.fbo, GL_COLOR_ATTACHMENT0, next.depth, mip);
        }
        success = success && checkAttachment(next.fbo, GL_COLOR_ATTACHMENT0,
                                              next.visibility[0], 0);
        success = success && checkAttachment(next.fbo, GL_COLOR_ATTACHMENT0,
                                              next.visibility[1], 0);
        success = success && checkAttachment(next.fbo, GL_COLOR_ATTACHMENT0,
                                              next.edges, 0);

        if (success)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, next.visibility[0], 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                                   GL_TEXTURE_2D, next.edges, 0);
            const GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, draw_buffers);
            success = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        glBindTexture(GL_TEXTURE_2D, saved_texture);
        next.valid = success;
        return success;
    }

    bool supportedFragment()
    {
        return gGLManager.mGLVersion >= 4.0f &&
            (gGLManager.mGLSLVersionMajor > 4 ||
             (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 0));
    }

    bool supportedCompute()
    {
        return gGLManager.mGLVersion >= 4.29f &&
            (gGLManager.mGLSLVersionMajor > 4 ||
             (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 30));
    }

    void configureFragmentShader(LLGLSLShader& shader, const std::string& name,
                                 const std::string& terminal, S32 shader_level)
    {
        shader.unload();
        shader.mName = name;
        shader.mFeatures.attachNothing = true;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
        shader.mShaderFiles.emplace_back(terminal, GL_FRAGMENT_SHADER);
        shader.mShaderLevel = shader_level;
    }

    void configureComputeShader(LLGLSLShader& shader, const std::string& name,
                                const std::string& terminal, S32 shader_level)
    {
        shader.unload();
        shader.mName = name;
        shader.mFeatures.attachNothing = true;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mShaderFiles.emplace_back("deferred/asGTAOCommonF.glsl", GL_COMPUTE_SHADER);
        shader.mShaderFiles.emplace_back(terminal, GL_COMPUTE_SHADER);
        shader.mShaderLevel = shader_level;
    }

    void bindRawSampler(LLGLSLShader& shader, const LLStaticHashedString& uniform,
                        S32 channel, GLuint texture, bool has_mips = false)
    {
        shader.uniform1i(uniform, channel);
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, texture, has_mips);
        gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    void unbindRawSampler(S32 channel)
    {
        gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
    }

    void attachSingle(GLuint texture, GLint mip, U32 width, U32 height)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, mip);
        const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_buffer);
        glViewport(0, 0, width, height);
    }

    void drawTriangle(LLVertexBuffer& triangle)
    {
        triangle.setBuffer();
        triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    // The reference beta is intentionally sharp. Use progressively lower
    // center weights for the explicitly softer modes so large AO radii do not
    // leave the screen-space Hilbert sampling pattern visible.
    F32 denoiseBlurBeta(S32 pass_count)
    {
        if (pass_count >= 3) return 0.35f;
        if (pass_count == 2) return 0.7f;
        return 1.2f;
    }

    bool bentNormalsRequested()
    {
        return gSavedSettings.getBOOL("RenderGTAOBentNormals");
    }

    void applyMainUniforms(LLGLSLShader& shader, const glm::mat4& projection,
                           U32 width, U32 height, F32 radius, F32 power, bool orthographic)
    {
        shader.uniform2f(U_PIXEL_SIZE, 1.f / width, 1.f / height);
        const F32 inv_x = 1.f / projection[0][0];
        const F32 inv_y = 1.f / projection[1][1];
        shader.uniform2f(U_NDC_MUL, 2.f * inv_x, -2.f * inv_y);
        if (orthographic)
        {
            shader.uniform2f(U_NDC_ADD,
                             (-1.f - projection[3][0]) * inv_x,
                             (1.f + projection[3][1]) * inv_y);
        }
        else
        {
            shader.uniform2f(U_NDC_ADD,
                             (-1.f + projection[2][0]) * inv_x,
                             (1.f - projection[2][1]) * inv_y);
        }
        shader.uniform1f(U_RADIUS, radius);
        shader.uniform1f(U_POWER, power);
        shader.uniform1i(U_ORTHOGRAPHIC_VIEW, orthographic ? 1 : 0);
    }

    bool renderFragment(LLRenderTarget& deferred_screen, LLVertexBuffer& triangle)
    {
        clearGLErrors();
        const U32 width = deferred_screen.getWidth();
        const U32 height = deferred_screen.getHeight();
        const GLuint saved_fbo = LLRenderTarget::sCurFBO;
        const U32 saved_width = LLRenderTarget::sCurResX;
        const U32 saved_height = LLRenderTarget::sCurResY;

        LLGLDepthTest depth_state(GL_FALSE, GL_FALSE);
        LLGLDisable blend_state(GL_BLEND);
        gGL.setColorMask(true, false);
        glBindFramebuffer(GL_FRAMEBUFFER, sResources.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D, 0, 0);

        const glm::mat4& projection = get_current_projection();
        const bool orthographic = fabsf(projection[3][3]) > 0.5f;
        const F32 radius = llclamp(gSavedSettings.getF32("RenderGTAORadius"), 0.05f, 5.f);
        const F32 power = llclamp(gSavedSettings.getF32("RenderGTAOFinalValuePower"), 0.5f, 5.f);

        {
            LL_PROFILE_GPU_ZONE("GTAO depth");
            attachSingle(sResources.depth, 0, width, height);
            sDepthProgram.bind();
            bindRawSampler(sDepthProgram, U_SOURCE_DEPTH, 0, deferred_screen.getDepth());
            sDepthProgram.uniform2f(U_PROJECTION_DEPTH, projection[2][2], projection[3][2]);
            sDepthProgram.uniform1i(U_ORTHOGRAPHIC, orthographic ? 1 : 0);
            drawTriangle(triangle);
            unbindRawSampler(0);
            sDepthProgram.unbind();
        }

        {
            LL_PROFILE_GPU_ZONE("GTAO depth mips");
            sDownsampleProgram.bind();
            bindRawSampler(sDownsampleProgram, LLStaticHashedString("gtao_source_depth"),
                           0, sResources.depth, true);
            sDownsampleProgram.uniform1f(U_RADIUS, radius);
            U32 source_width = width;
            U32 source_height = height;
            for (U32 mip = 1; mip < GTAO_MIP_COUNT; ++mip)
            {
                const U32 mip_width = llmax(1u, width >> mip);
                const U32 mip_height = llmax(1u, height >> mip);
                // Restrict sampling to the source mip so the attached destination
                // mip is not part of the texture feedback loop.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, mip - 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip - 1);
                attachSingle(sResources.depth, mip, mip_width, mip_height);
                sDownsampleProgram.uniform1i(U_SOURCE_MIP, 0);
                sDownsampleProgram.uniform2i(U_SOURCE_SIZE, source_width, source_height);
                drawTriangle(triangle);
                source_width = mip_width;
                source_height = mip_height;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GTAO_MIP_COUNT - 1);
            unbindRawSampler(0);
            sDownsampleProgram.unbind();
        }

        {
            LL_PROFILE_GPU_ZONE("GTAO main");
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   sResources.visibility[0], 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                                   sResources.edges, 0);
            const GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, draw_buffers);
            glViewport(0, 0, width, height);
            gGL.setColorMask(true, true);

            const S32 quality = llclamp(gSavedSettings.getS32("RenderGTAOQuality"), 1, 4);
            LLGLSLShader& shader = sResources.bent_normals
                ? (quality == 1 ? sBentMainMediumProgram :
                   quality == 2 ? sBentMainHighProgram :
                   quality == 3 ? sBentMainUltraProgram : sBentMainCinematicProgram)
                : (quality == 1 ? sMainMediumProgram :
                   quality == 2 ? sMainHighProgram :
                   quality == 3 ? sMainUltraProgram : sMainCinematicProgram);
            shader.bind();
            bindRawSampler(shader, LLStaticHashedString("gtao_depth"), 0,
                           sResources.depth, true);
            bindRawSampler(shader, LLStaticHashedString("gtao_normal"), 1,
                           deferred_screen.getTexture(2));
            bindRawSampler(shader, LLStaticHashedString("gtao_hilbert"), 2, sResources.hilbert);
            applyMainUniforms(shader, projection, width, height, radius, power, orthographic);
            drawTriangle(triangle);
            unbindRawSampler(2);
            unbindRawSampler(1);
            unbindRawSampler(0);
            shader.unbind();
        }

        U32 read_index = 0;
        U32 write_index = 1;
        const S32 configured_passes = llclamp(gSavedSettings.getS32("RenderGTAODenoisePasses"), 0, 3);
        const S32 pass_count = llmax(1, configured_passes);
        {
            LL_PROFILE_GPU_ZONE("GTAO denoise");
            LLGLSLShader& denoise_shader = sResources.bent_normals
                ? sBentDenoiseProgram : sDenoiseProgram;
            denoise_shader.bind();
            denoise_shader.uniform1f(U_BLUR_BETA, configured_passes == 0
                ? 10000.f : denoiseBlurBeta(configured_passes));
            bindRawSampler(denoise_shader, LLStaticHashedString("gtao_edges_source"),
                           1, sResources.edges);
            for (S32 pass = 0; pass < pass_count; ++pass)
            {
                attachSingle(sResources.visibility[write_index], 0, width, height);
                bindRawSampler(denoise_shader,
                               LLStaticHashedString("gtao_visibility_source"),
                               0, sResources.visibility[read_index]);
                denoise_shader.uniform1i(U_FINAL_PASS, pass == pass_count - 1 ? 1 : 0);
                drawTriangle(triangle);
                unbindRawSampler(0);
                std::swap(read_index, write_index);
            }
            unbindRawSampler(1);
            denoise_shader.unbind();
        }

        sFinalVisibility = sResources.visibility[read_index];
        glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
        glViewport(0, 0, saved_width, saved_height);
        LLRenderTarget::sCurFBO = saved_fbo;
        LLRenderTarget::sCurResX = saved_width;
        LLRenderTarget::sCurResY = saved_height;
        gGL.setColorMask(true, true);
        return glGetError() == GL_NO_ERROR;
    }

    void saveImageBindings(std::array<ImageBinding, GTAO_MIP_COUNT>& bindings)
    {
        for (GLuint binding = 0; binding < GTAO_MIP_COUNT; ++binding)
        {
            glGetIntegeri_v(GL_IMAGE_BINDING_NAME, binding, &bindings[binding].texture);
            glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, binding, &bindings[binding].level);
            glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, binding, &bindings[binding].layered);
            glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, binding, &bindings[binding].layer);
            glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, binding, &bindings[binding].access);
            glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, binding, &bindings[binding].format);
        }
    }

    void restoreImageBindings(const std::array<ImageBinding, GTAO_MIP_COUNT>& bindings)
    {
        for (GLuint binding = 0; binding < GTAO_MIP_COUNT; ++binding)
        {
            const ImageBinding& saved = bindings[binding];
            glBindImageTexture(binding, saved.texture, saved.level, saved.layered,
                               saved.layer, saved.access,
                               saved.texture ? saved.format : GL_R8);
        }
    }

    bool renderCompute(LLRenderTarget& deferred_screen)
    {
        clearGLErrors();
        std::array<ImageBinding, GTAO_MIP_COUNT> saved_images;
        saveImageBindings(saved_images);
        const U32 width = deferred_screen.getWidth();
        const U32 height = deferred_screen.getHeight();
        const glm::mat4& projection = get_current_projection();
        const bool orthographic = fabsf(projection[3][3]) > 0.5f;
        const F32 radius = llclamp(gSavedSettings.getF32("RenderGTAORadius"), 0.05f, 5.f);
        const F32 power = llclamp(gSavedSettings.getF32("RenderGTAOFinalValuePower"), 0.5f, 5.f);

        {
            LL_PROFILE_GPU_ZONE("GTAO compute depth");
            sComputeDepthProgram.bind();
            bindRawSampler(sComputeDepthProgram, U_SOURCE_DEPTH, 0, deferred_screen.getDepth());
            sComputeDepthProgram.uniform2f(U_PROJECTION_DEPTH,
                                           projection[2][2], projection[3][2]);
            sComputeDepthProgram.uniform1i(U_ORTHOGRAPHIC, orthographic ? 1 : 0);
            sComputeDepthProgram.uniform1f(U_RADIUS, radius);
            sComputeDepthProgram.uniform2i(U_VIEWPORT, width, height);
            for (GLuint mip = 0; mip < GTAO_MIP_COUNT; ++mip)
            {
                glBindImageTexture(mip, sResources.depth, mip, GL_FALSE, 0,
                                   GL_WRITE_ONLY, GL_R16F);
            }
            glDispatchCompute((width + 15) / 16, (height + 15) / 16, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            GL_TEXTURE_FETCH_BARRIER_BIT);
            unbindRawSampler(0);
            sComputeDepthProgram.unbind();
        }

        {
            LL_PROFILE_GPU_ZONE("GTAO compute main");
            const S32 quality = llclamp(gSavedSettings.getS32("RenderGTAOQuality"), 1, 4);
            LLGLSLShader& shader = sResources.bent_normals
                ? (quality == 1 ? sComputeBentMainMediumProgram :
                   quality == 2 ? sComputeBentMainHighProgram :
                   quality == 3 ? sComputeBentMainUltraProgram :
                                  sComputeBentMainCinematicProgram)
                : (quality == 1 ? sComputeMainMediumProgram :
                   quality == 2 ? sComputeMainHighProgram :
                   quality == 3 ? sComputeMainUltraProgram :
                                  sComputeMainCinematicProgram);
            shader.bind();
            bindRawSampler(shader, LLStaticHashedString("gtao_depth"), 0,
                           sResources.depth, true);
            bindRawSampler(shader, LLStaticHashedString("gtao_normal"), 1,
                           deferred_screen.getTexture(2));
            bindRawSampler(shader, LLStaticHashedString("gtao_hilbert"), 2, sResources.hilbert);
            applyMainUniforms(shader, projection, width, height, radius, power, orthographic);
            shader.uniform2i(U_VIEWPORT, width, height);
            glBindImageTexture(0, sResources.visibility[0], 0, GL_FALSE, 0,
                               GL_WRITE_ONLY, sResources.bent_normals ? GL_RGBA8 : GL_R8);
            glBindImageTexture(1, sResources.edges, 0, GL_FALSE, 0,
                               GL_WRITE_ONLY, GL_R8);
            glDispatchCompute((width + 7) / 8, (height + 7) / 8, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            GL_TEXTURE_FETCH_BARRIER_BIT);
            unbindRawSampler(2);
            unbindRawSampler(1);
            unbindRawSampler(0);
            shader.unbind();
        }

        U32 read_index = 0;
        U32 write_index = 1;
        const S32 configured_passes = llclamp(
            gSavedSettings.getS32("RenderGTAODenoisePasses"), 0, 3);
        const S32 pass_count = llmax(1, configured_passes);
        {
            LL_PROFILE_GPU_ZONE("GTAO compute denoise");
            LLGLSLShader& denoise_shader = sResources.bent_normals
                ? sComputeBentDenoiseProgram : sComputeDenoiseProgram;
            denoise_shader.bind();
            denoise_shader.uniform1f(U_BLUR_BETA,
                configured_passes == 0 ? 10000.f : denoiseBlurBeta(configured_passes));
            denoise_shader.uniform2i(U_VIEWPORT, width, height);
            bindRawSampler(denoise_shader,
                           LLStaticHashedString("gtao_edges_source"), 1, sResources.edges);
            for (S32 pass = 0; pass < pass_count; ++pass)
            {
                bindRawSampler(denoise_shader,
                               LLStaticHashedString("gtao_visibility_source"),
                               0, sResources.visibility[read_index]);
                denoise_shader.uniform1i(U_FINAL_PASS,
                                         pass == pass_count - 1 ? 1 : 0);
                glBindImageTexture(0, sResources.visibility[write_index], 0,
                                   GL_FALSE, 0, GL_WRITE_ONLY,
                                   sResources.bent_normals ? GL_RGBA8 : GL_R8);
                glDispatchCompute((width + 15) / 16, (height + 7) / 8, 1);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                GL_TEXTURE_FETCH_BARRIER_BIT);
                unbindRawSampler(0);
                std::swap(read_index, write_index);
            }
            unbindRawSampler(1);
            denoise_shader.unbind();
        }

        restoreImageBindings(saved_images);
        sFinalVisibility = sResources.visibility[read_index];
        return glGetError() == GL_NO_ERROR;
    }
}

const char* ASAmbientOcclusion::shaderCacheRevision()
{
    return "as-ambient-occlusion-v3-cinematic";
}

ASAmbientOcclusion::Technique ASAmbientOcclusion::requestedTechnique()
{
    return gSavedSettings.getS32("RenderAOTechnique") == GTAO ? GTAO : LEGACY_SSAO;
}

ASAmbientOcclusion::Technique ASAmbientOcclusion::effectiveTechnique()
{
    return sEffectiveTechnique;
}

ASAmbientOcclusion::Backend ASAmbientOcclusion::requestedBackend()
{
    return (Backend)llclamp(gSavedSettings.getS32("RenderGTAOBackend"), 0, 2);
}

ASAmbientOcclusion::Backend ASAmbientOcclusion::effectiveBackend()
{
    return sEffectiveBackend;
}

bool ASAmbientOcclusion::debugWhiteEnabled()
{
    return gSavedSettings.getBOOL("RenderDeferredSSAO") &&
        gSavedSettings.getBOOL("RenderAODebugWhite");
}

void ASAmbientOcclusion::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sDepthProgram);
    shaders.push_back(&sDownsampleProgram);
    shaders.push_back(&sMainMediumProgram);
    shaders.push_back(&sMainHighProgram);
    shaders.push_back(&sMainUltraProgram);
    shaders.push_back(&sMainCinematicProgram);
    shaders.push_back(&sDenoiseProgram);
    shaders.push_back(&sBentMainMediumProgram);
    shaders.push_back(&sBentMainHighProgram);
    shaders.push_back(&sBentMainUltraProgram);
    shaders.push_back(&sBentMainCinematicProgram);
    shaders.push_back(&sBentDenoiseProgram);
    shaders.push_back(&sComputeDepthProgram);
    shaders.push_back(&sComputeMainMediumProgram);
    shaders.push_back(&sComputeMainHighProgram);
    shaders.push_back(&sComputeMainUltraProgram);
    shaders.push_back(&sComputeMainCinematicProgram);
    shaders.push_back(&sComputeDenoiseProgram);
    shaders.push_back(&sComputeBentMainMediumProgram);
    shaders.push_back(&sComputeBentMainHighProgram);
    shaders.push_back(&sComputeBentMainUltraProgram);
    shaders.push_back(&sComputeBentMainCinematicProgram);
    shaders.push_back(&sComputeBentDenoiseProgram);
}

bool ASAmbientOcclusion::loadShaders(S32 shader_level)
{
    unloadShaders();
    if (!supportedFragment())
    {
        return true;
    }

    configureFragmentShader(sDepthProgram, "AS GTAO Depth", "deferred/asGTAODepthF.glsl",
                            shader_level);
    configureFragmentShader(sDownsampleProgram, "AS GTAO Depth Downsample",
                            "deferred/asGTAODownsampleF.glsl", shader_level);
    sDownsampleProgram.mShaderFiles.insert(sDownsampleProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    configureFragmentShader(sMainMediumProgram, "AS GTAO Main Medium",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sMainMediumProgram.mShaderFiles.insert(sMainMediumProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sMainMediumProgram.addPermutation("GTAO_SLICES", "2");
    sMainMediumProgram.addPermutation("GTAO_STEPS", "2");
    configureFragmentShader(sMainHighProgram, "AS GTAO Main High",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sMainHighProgram.mShaderFiles.insert(sMainHighProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sMainHighProgram.addPermutation("GTAO_SLICES", "3");
    sMainHighProgram.addPermutation("GTAO_STEPS", "3");
    configureFragmentShader(sMainUltraProgram, "AS GTAO Main Ultra",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sMainUltraProgram.mShaderFiles.insert(sMainUltraProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sMainUltraProgram.addPermutation("GTAO_SLICES", "9");
    sMainUltraProgram.addPermutation("GTAO_STEPS", "3");
    configureFragmentShader(sMainCinematicProgram, "AS GTAO Main Cinematic",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sMainCinematicProgram.mShaderFiles.insert(sMainCinematicProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sMainCinematicProgram.addPermutation("GTAO_SLICES", "18");
    sMainCinematicProgram.addPermutation("GTAO_STEPS", "4");
    configureFragmentShader(sDenoiseProgram, "AS GTAO Denoise",
                            "deferred/asGTAODenoiseF.glsl", shader_level);
    sDenoiseProgram.mShaderFiles.insert(sDenoiseProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));

    configureFragmentShader(sBentMainMediumProgram, "AS GTAO Bent Main Medium",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sBentMainMediumProgram.mShaderFiles.insert(sBentMainMediumProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sBentMainMediumProgram.addPermutation("GTAO_SLICES", "2");
    sBentMainMediumProgram.addPermutation("GTAO_STEPS", "2");
    sBentMainMediumProgram.addPermutation("GTAO_BENT_NORMALS", "1");
    configureFragmentShader(sBentMainHighProgram, "AS GTAO Bent Main High",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sBentMainHighProgram.mShaderFiles.insert(sBentMainHighProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sBentMainHighProgram.addPermutation("GTAO_SLICES", "3");
    sBentMainHighProgram.addPermutation("GTAO_STEPS", "3");
    sBentMainHighProgram.addPermutation("GTAO_BENT_NORMALS", "1");
    configureFragmentShader(sBentMainUltraProgram, "AS GTAO Bent Main Ultra",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sBentMainUltraProgram.mShaderFiles.insert(sBentMainUltraProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sBentMainUltraProgram.addPermutation("GTAO_SLICES", "9");
    sBentMainUltraProgram.addPermutation("GTAO_STEPS", "3");
    sBentMainUltraProgram.addPermutation("GTAO_BENT_NORMALS", "1");
    configureFragmentShader(sBentMainCinematicProgram, "AS GTAO Bent Main Cinematic",
                            "deferred/asGTAOMainF.glsl", shader_level);
    sBentMainCinematicProgram.mShaderFiles.insert(
        sBentMainCinematicProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sBentMainCinematicProgram.addPermutation("GTAO_SLICES", "18");
    sBentMainCinematicProgram.addPermutation("GTAO_STEPS", "4");
    sBentMainCinematicProgram.addPermutation("GTAO_BENT_NORMALS", "1");
    configureFragmentShader(sBentDenoiseProgram, "AS GTAO Bent Denoise",
                            "deferred/asGTAODenoiseF.glsl", shader_level);
    sBentDenoiseProgram.mShaderFiles.insert(sBentDenoiseProgram.mShaderFiles.end() - 1,
        std::make_pair("deferred/asGTAOCommonF.glsl", GL_FRAGMENT_SHADER));
    sBentDenoiseProgram.addPermutation("GTAO_BENT_NORMALS", "1");

    sFragmentShadersLoaded = sDepthProgram.createShader() &&
        sDownsampleProgram.createShader() && sMainMediumProgram.createShader() &&
        sMainHighProgram.createShader() && sMainUltraProgram.createShader() &&
        sMainCinematicProgram.createShader() && sDenoiseProgram.createShader() &&
        sBentMainMediumProgram.createShader() &&
        sBentMainHighProgram.createShader() && sBentMainUltraProgram.createShader() &&
        sBentMainCinematicProgram.createShader() && sBentDenoiseProgram.createShader();
    if (!sFragmentShadersLoaded)
    {
        unloadShaders();
        LL_WARNS("GTAO") << "Optional GTAO fragment shaders failed; compute may remain available."
                          << LL_ENDL;
    }

    if (supportedCompute())
    {
        configureComputeShader(sComputeDepthProgram, "AS GTAO Compute Depth",
                               "deferred/asGTAODepthC.glsl", shader_level);
        configureComputeShader(sComputeMainMediumProgram, "AS GTAO Compute Main Medium",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeMainMediumProgram.addPermutation("GTAO_SLICES", "2");
        sComputeMainMediumProgram.addPermutation("GTAO_STEPS", "2");
        configureComputeShader(sComputeMainHighProgram, "AS GTAO Compute Main High",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeMainHighProgram.addPermutation("GTAO_SLICES", "3");
        sComputeMainHighProgram.addPermutation("GTAO_STEPS", "3");
        configureComputeShader(sComputeMainUltraProgram, "AS GTAO Compute Main Ultra",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeMainUltraProgram.addPermutation("GTAO_SLICES", "9");
        sComputeMainUltraProgram.addPermutation("GTAO_STEPS", "3");
        configureComputeShader(sComputeMainCinematicProgram,
                               "AS GTAO Compute Main Cinematic",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeMainCinematicProgram.addPermutation("GTAO_SLICES", "18");
        sComputeMainCinematicProgram.addPermutation("GTAO_STEPS", "4");
        configureComputeShader(sComputeDenoiseProgram, "AS GTAO Compute Denoise",
                               "deferred/asGTAODenoiseC.glsl", shader_level);

        configureComputeShader(sComputeBentMainMediumProgram,
                               "AS GTAO Compute Bent Main Medium",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeBentMainMediumProgram.addPermutation("GTAO_SLICES", "2");
        sComputeBentMainMediumProgram.addPermutation("GTAO_STEPS", "2");
        sComputeBentMainMediumProgram.addPermutation("GTAO_BENT_NORMALS", "1");
        configureComputeShader(sComputeBentMainHighProgram,
                               "AS GTAO Compute Bent Main High",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeBentMainHighProgram.addPermutation("GTAO_SLICES", "3");
        sComputeBentMainHighProgram.addPermutation("GTAO_STEPS", "3");
        sComputeBentMainHighProgram.addPermutation("GTAO_BENT_NORMALS", "1");
        configureComputeShader(sComputeBentMainUltraProgram,
                               "AS GTAO Compute Bent Main Ultra",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeBentMainUltraProgram.addPermutation("GTAO_SLICES", "9");
        sComputeBentMainUltraProgram.addPermutation("GTAO_STEPS", "3");
        sComputeBentMainUltraProgram.addPermutation("GTAO_BENT_NORMALS", "1");
        configureComputeShader(sComputeBentMainCinematicProgram,
                               "AS GTAO Compute Bent Main Cinematic",
                               "deferred/asGTAOMainC.glsl", shader_level);
        sComputeBentMainCinematicProgram.addPermutation("GTAO_SLICES", "18");
        sComputeBentMainCinematicProgram.addPermutation("GTAO_STEPS", "4");
        sComputeBentMainCinematicProgram.addPermutation("GTAO_BENT_NORMALS", "1");
        configureComputeShader(sComputeBentDenoiseProgram,
                               "AS GTAO Compute Bent Denoise",
                               "deferred/asGTAODenoiseC.glsl", shader_level);
        sComputeBentDenoiseProgram.addPermutation("GTAO_BENT_NORMALS", "1");
        sComputeShadersLoaded = sComputeDepthProgram.createShader() &&
            sComputeMainMediumProgram.createShader() && sComputeMainHighProgram.createShader() &&
            sComputeMainUltraProgram.createShader() &&
            sComputeMainCinematicProgram.createShader() &&
            sComputeDenoiseProgram.createShader() &&
            sComputeBentMainMediumProgram.createShader() &&
            sComputeBentMainHighProgram.createShader() &&
            sComputeBentMainUltraProgram.createShader() &&
            sComputeBentMainCinematicProgram.createShader() &&
            sComputeBentDenoiseProgram.createShader();
        if (!sComputeShadersLoaded)
        {
            sComputeDepthProgram.unload();
            sComputeMainMediumProgram.unload();
            sComputeMainHighProgram.unload();
            sComputeMainUltraProgram.unload();
            sComputeMainCinematicProgram.unload();
            sComputeDenoiseProgram.unload();
            sComputeBentMainMediumProgram.unload();
            sComputeBentMainHighProgram.unload();
            sComputeBentMainUltraProgram.unload();
            sComputeBentMainCinematicProgram.unload();
            sComputeBentDenoiseProgram.unload();
            LL_WARNS("GTAO") << "Optional GTAO compute shaders failed; using fragment backend."
                              << LL_ENDL;
        }
    }
    return true;
}

void ASAmbientOcclusion::unloadShaders()
{
    sDepthProgram.unload();
    sDownsampleProgram.unload();
    sMainMediumProgram.unload();
    sMainHighProgram.unload();
    sMainUltraProgram.unload();
    sMainCinematicProgram.unload();
    sDenoiseProgram.unload();
    sBentMainMediumProgram.unload();
    sBentMainHighProgram.unload();
    sBentMainUltraProgram.unload();
    sBentMainCinematicProgram.unload();
    sBentDenoiseProgram.unload();
    sComputeDepthProgram.unload();
    sComputeMainMediumProgram.unload();
    sComputeMainHighProgram.unload();
    sComputeMainUltraProgram.unload();
    sComputeMainCinematicProgram.unload();
    sComputeDenoiseProgram.unload();
    sComputeBentMainMediumProgram.unload();
    sComputeBentMainHighProgram.unload();
    sComputeBentMainUltraProgram.unload();
    sComputeBentMainCinematicProgram.unload();
    sComputeBentDenoiseProgram.unload();
    sFragmentShadersLoaded = false;
    sComputeShadersLoaded = false;
    sFrameValid = false;
    sFrameHasBentNormals = false;
}

void ASAmbientOcclusion::allocateResources(U32 width, U32 height)
{
    if (requestedTechnique() != GTAO || (!sFragmentShadersLoaded && !sComputeShadersLoaded) ||
        width == 0 || height == 0)
    {
        return;
    }
    const bool bent_normals = bentNormalsRequested();
    if (sResources.valid && sResources.width == width && sResources.height == height &&
        sResources.bent_normals == bent_normals)
    {
        return;
    }

    Resources next;
    if (!allocateNewResources(next, width, height, bent_normals))
    {
        destroyResources(next);
        if (!sWarnedAllocation)
        {
            LL_WARNS("GTAO") << "GTAO resource allocation failed; retaining legacy SSAO."
                              << LL_ENDL;
            sWarnedAllocation = true;
        }
        return;
    }

    destroyResources(sResources);
    sResources = next;
    sFrameValid = false;
    sFrameHasBentNormals = false;
    sFinalVisibility = 0;
    sWarnedAllocation = false;
    LL_INFOS("GTAO") << "Allocated " << width << "x" << height << " GTAO targets in "
                       << (bent_normals ? "bent-normal RGBA8" : "scalar R8")
                       << " mode." << LL_ENDL;
}

void ASAmbientOcclusion::releaseResources()
{
    destroyResources(sResources);
    sFrameValid = false;
    sFrameHasBentNormals = false;
    sFinalVisibility = 0;
}

bool ASAmbientOcclusion::render(LLRenderTarget& deferred_screen,
                                LLVertexBuffer& screen_triangle)
{
    LL_PROFILE_GPU_ZONE("GTAO total");
    sFrameValid = false;
    sFrameHasBentNormals = false;
    sEffectiveTechnique = LEGACY_SSAO;
    sEffectiveBackend = AUTO;

    if (requestedTechnique() != GTAO || (!sFragmentShadersLoaded && !sComputeShadersLoaded))
    {
        return false;
    }

    allocateResources(deferred_screen.getWidth(), deferred_screen.getHeight());
    if (!sResources.valid || sResources.width != deferred_screen.getWidth() ||
        sResources.height != deferred_screen.getHeight() ||
        sResources.bent_normals != bentNormalsRequested())
    {
        // Never retain a result from the opposite mode if replacement target
        // allocation failed during a live checkbox transition.
        sFinalVisibility = 0;
        return false;
    }

    const Backend requested = requestedBackend();
    const bool compute_available = supportedCompute() && sComputeShadersLoaded;
    if (requested == COMPUTE && !compute_available && !sWarnedForcedCompute)
    {
        LL_WARNS("GTAO") << "Forced GTAO compute backend is unavailable; using fragment backend."
                          << LL_ENDL;
        sWarnedForcedCompute = true;
    }

    const bool use_compute = compute_available && requested != FRAGMENT;
    if (!use_compute && !sFragmentShadersLoaded)
    {
        return false;
    }
    const U32 saved_active_unit = gGL.getCurrentTexUnitIndex();
    std::array<U32, 3> saved_textures;
    std::array<LLTexUnit::eTextureType, 3> saved_types;
    for (S32 channel = 0; channel < 3; ++channel)
    {
        saved_textures[channel] = gGL.getTexUnit(channel)->getCurrTexture();
        saved_types[channel] = gGL.getTexUnit(channel)->getCurrType();
    }

    bool rendered_with_compute = use_compute;
    sFrameValid = use_compute
        ? renderCompute(deferred_screen)
        : renderFragment(deferred_screen, screen_triangle);
    if (!sFrameValid && use_compute && sFragmentShadersLoaded)
    {
        LL_WARNS_ONCE("GTAO") << "GTAO compute rendering failed; trying fragment backend."
                               << LL_ENDL;
        clearGLErrors();
        rendered_with_compute = false;
        sFrameValid = renderFragment(deferred_screen, screen_triangle);
    }
    for (S32 channel = 0; channel < 3; ++channel)
    {
        if (saved_textures[channel])
        {
            gGL.getTexUnit(channel)->bindManual(saved_types[channel], saved_textures[channel]);
        }
        else
        {
            gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
        }
    }
    gGL.getTexUnit(saved_active_unit)->activate();
    if (sFrameValid)
    {
        sEffectiveTechnique = GTAO;
        sEffectiveBackend = rendered_with_compute ? COMPUTE : FRAGMENT;
        sFrameHasBentNormals = sResources.bent_normals;
        sWarnedRender = false;
        return true;
    }

    if (!sWarnedRender)
    {
        LL_WARNS("GTAO") << "GTAO rendering failed; using legacy SSAO for this frame."
                          << LL_ENDL;
        sWarnedRender = true;
    }
    return false;
}

bool ASAmbientOcclusion::bindResult(LLGLSLShader& shader)
{
    if (!sFrameValid || !sFinalVisibility || effectiveTechnique() != GTAO)
    {
        return false;
    }
    sBoundResultChannel = shader.mActiveTextureChannels;
    if (sBoundResultChannel < 0 || sBoundResultChannel >= gGLManager.mNumTextureImageUnits ||
        shader.getUniformLocation(U_RESULT) < 0)
    {
        sBoundResultChannel = -1;
        return false;
    }
    shader.uniform1i(U_RESULT, sBoundResultChannel);
    gGL.getTexUnit(sBoundResultChannel)->bindManual(LLTexUnit::TT_TEXTURE, sFinalVisibility);
    gGL.getTexUnit(sBoundResultChannel)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    gGL.getTexUnit(sBoundResultChannel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    return true;
}

void ASAmbientOcclusion::unbindResult(LLGLSLShader&)
{
    if (sBoundResultChannel >= 0)
    {
        gGL.getTexUnit(sBoundResultChannel)->unbind(LLTexUnit::TT_TEXTURE);
        sBoundResultChannel = -1;
    }
}

bool ASAmbientOcclusion::bentNormalsEffective()
{
    return sFrameValid && sFrameHasBentNormals && effectiveTechnique() == GTAO;
}
