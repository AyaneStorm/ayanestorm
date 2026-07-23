/**
 * @file fsavboit.cpp
 * @brief Approximate adaptive voxel-based OIT resolve.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include "fsavboit.h"

#include "llglslshader.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"

namespace
{
constexpr U32 AVBOIT_SCALE = 8;
constexpr U32 AVBOIT_SLICES = 128;
constexpr U32 AVBOIT_VIRTUAL_SLICES = 8192;

S32 directTransmittanceTextureUnit()
{
    return llmax(0, gGLManager.mNumTextureImageUnits - 1);
}

LLGLSLShader gAVBOITVolumeProgram;
LLGLSLShader gAVBOITResolveProgram;
LLRenderTarget gAVBOITOpaqueTarget;
}

FSAVBOIT::Resources FSAVBOIT::sResources;
S32 FSAVBOIT::sDirectRasterPass = -1;
bool FSAVBOIT::sDirectFrameReady = false;

bool FSAVBOIT::supported()
{
    return gGLManager.mGLVersion >= 4.29f &&
        (gGLManager.mGLSLVersionMajor > 4 ||
         (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 30));
}

bool FSAVBOIT::requested()
{
    return gSavedSettings.getBOOL("RenderAVBOIT") &&
        gSavedSettings.getS32("RenderExactOITDebugMode") == 0 && supported();
}

bool FSAVBOIT::available()
{
    return requested() && sResources.available &&
        gAVBOITVolumeProgram.mProgramObject && gAVBOITResolveProgram.mProgramObject;
}

void FSAVBOIT::loadShaders(S32 shader_level)
{
    if (!supported())
    {
        return;
    }

    gAVBOITVolumeProgram.mName = "AVBOIT Volume Compute";
    gAVBOITVolumeProgram.mFeatures.attachNothing = true;
    gAVBOITVolumeProgram.mShaderFiles.clear();
    gAVBOITVolumeProgram.mShaderFiles.emplace_back("deferred/avboitVolumeC.glsl", GL_COMPUTE_SHADER);
    gAVBOITVolumeProgram.mShaderLevel = shader_level;
    gAVBOITVolumeProgram.clearPermutations();
    gAVBOITVolumeProgram.addPermutation("AVBOIT_BUILD", "1");

    gAVBOITResolveProgram.mName = "AVBOIT Resolve Compute";
    gAVBOITResolveProgram.mFeatures.attachNothing = true;
    gAVBOITResolveProgram.mShaderFiles.clear();
    gAVBOITResolveProgram.mShaderFiles.emplace_back("deferred/avboitVolumeC.glsl", GL_COMPUTE_SHADER);
    gAVBOITResolveProgram.mShaderLevel = shader_level;
    gAVBOITResolveProgram.clearPermutations();
    gAVBOITResolveProgram.addPermutation("AVBOIT_RESOLVE", "1");

    if (!gAVBOITVolumeProgram.createShader() || !gAVBOITResolveProgram.createShader())
    {
        unloadShaders();
        LL_WARNS("AVBOIT") << "AVBOIT shaders unavailable; Exact OIT fallback remains active"
                            << LL_ENDL;
    }
}

void FSAVBOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list)
{
    shader_list.push_back(&gAVBOITVolumeProgram);
    shader_list.push_back(&gAVBOITResolveProgram);
}

void FSAVBOIT::unloadShaders()
{
    gAVBOITVolumeProgram.unload();
    gAVBOITResolveProgram.unload();
}

bool FSAVBOIT::allocateVolume(U32 width, U32 height)
{
    sResources.viewportWidth = width;
    sResources.viewportHeight = height;
    sResources.volumeWidth = (width + AVBOIT_SCALE - 1u) / AVBOIT_SCALE;
    sResources.volumeHeight = (height + AVBOIT_SCALE - 1u) / AVBOIT_SCALE;

    glGenTextures(1, &sResources.extinction);
    glBindTexture(GL_TEXTURE_3D, sResources.extinction);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32UI,
                   sResources.volumeWidth, sResources.volumeHeight, AVBOIT_SLICES);

    glGenTextures(1, &sResources.transmittance);
    glBindTexture(GL_TEXTURE_3D, sResources.transmittance);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32F,
                   sResources.volumeWidth, sResources.volumeHeight, AVBOIT_SLICES);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

    glGenTextures(1, &sResources.classification);
    glBindTexture(GL_TEXTURE_2D, sResources.classification);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, width, height);

    glGenTextures(1, &sResources.zeroTransmittanceDepth);
    glBindTexture(GL_TEXTURE_2D, sResources.zeroTransmittanceDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI,
                   sResources.volumeWidth, sResources.volumeHeight);

    glGenTextures(1, &sResources.totalTransmittance);
    glBindTexture(GL_TEXTURE_2D, sResources.totalTransmittance);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F,
                   sResources.volumeWidth, sResources.volumeHeight);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(1, &sResources.occupancy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 AVBOIT_VIRTUAL_SLICES * sizeof(U32), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.warp);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.warp);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 AVBOIT_VIRTUAL_SLICES * sizeof(U32), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.tileOccupancy);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
                     (AVBOIT_SLICES / 32u) * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.accumulation);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.accumulation);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<U64>(width) * height * 6u * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return glGetError() == GL_NO_ERROR &&
        gAVBOITOpaqueTarget.allocate(width, height, GL_RGBA16F);
}

void FSAVBOIT::allocateResources(U32 width, U32 height)
{
    releaseResources();
    if (requested())
    {
        sResources.available = allocateVolume(width, height);
        if (!sResources.available)
        {
            releaseResources();
        }
    }
}

void FSAVBOIT::releaseResources()
{
    if (sResources.extinction) glDeleteTextures(1, &sResources.extinction);
    if (sResources.transmittance) glDeleteTextures(1, &sResources.transmittance);
    if (sResources.classification) glDeleteTextures(1, &sResources.classification);
    if (sResources.zeroTransmittanceDepth)
        glDeleteTextures(1, &sResources.zeroTransmittanceDepth);
    if (sResources.totalTransmittance) glDeleteTextures(1, &sResources.totalTransmittance);
    if (sResources.occupancy) glDeleteBuffers(1, &sResources.occupancy);
    if (sResources.warp) glDeleteBuffers(1, &sResources.warp);
    if (sResources.tileOccupancy) glDeleteBuffers(1, &sResources.tileOccupancy);
    if (sResources.accumulation) glDeleteBuffers(1, &sResources.accumulation);
    gAVBOITOpaqueTarget.release();
    sDirectRasterPass = -1;
    sDirectFrameReady = false;
    sResources = Resources();
}

void FSAVBOIT::appendDiagnostics(LLSD& info)
{
    info["AVBOIT_AVAILABLE"] = available();
    info["AVBOIT_VOLUME_WIDTH"] = LLSD::Integer(sResources.volumeWidth);
    info["AVBOIT_VOLUME_HEIGHT"] = LLSD::Integer(sResources.volumeHeight);
    info["AVBOIT_SLICES"] = LLSD::Integer(AVBOIT_SLICES);
    info["AVBOIT_DIRECT_RASTER"] = sDirectRasterPass >= 0 || sDirectFrameReady;
    info["AVBOIT_ACCUMULATION_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.viewportWidth) * sResources.viewportHeight *
         6ull * sizeof(U32)) / (1024ull * 1024ull));
    info["AVBOIT_STATUS"] = !supported() ? "Unavailable: OpenGL 4.3 is required" :
        !available() ? "Unavailable or disabled; Exact OIT fallback active" :
        "Available";
}

bool FSAVBOIT::beginDirectFrame(LLRenderTarget& screen)
{
    const U32 width = screen.getWidth();
    const U32 height = screen.getHeight();
    if (requested() && (!sResources.available ||
        sResources.viewportWidth != width || sResources.viewportHeight != height))
    {
        const GLint previous_fbo = LLRenderTarget::sCurFBO;
        allocateResources(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
    }
    if (!available() || !sResources.accumulation || !gAVBOITOpaqueTarget.isComplete())
    {
        return false;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LL_INFOS("AVBOIT") << "Using independent direct-raster AVBOIT; Exact OIT nodes are not captured"
                            << LL_ENDL;
    }

    glCopyImageSubData(screen.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       gAVBOITOpaqueTarget.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       width, height, 1);

    const U32 zero = 0u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glBindImageTexture(3, sResources.extinction, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(4, sResources.transmittance, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R32F);
    glBindImageTexture(6, sResources.zeroTransmittanceDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(7, sResources.totalTransmittance, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R32F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sResources.occupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sResources.warp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, sResources.tileOccupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, sResources.accumulation);

    sDirectFrameReady = false;
    beginDirectRasterPass(0);
    return true;
}

void FSAVBOIT::beginDirectRasterPass(S32 pass)
{
    sDirectRasterPass = pass;
    if (pass == 2)
    {
        const U32 zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.accumulation);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                          GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        gGL.getTexUnit(directTransmittanceTextureUnit())->bindManual(
            LLTexUnit::TT_TEXTURE_3D, sResources.transmittance);
        sDirectFrameReady = true;
    }
}

void FSAVBOIT::configureDirectRasterShader(LLGLSLShader* shader)
{
    if (!shader)
    {
        return;
    }

    static LLStaticHashedString raster_pass("avboitRasterPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString transmittance_sampler("avboitTransmittanceSampler");
    GLint location = shader->getUniformLocation(raster_pass);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location, sDirectRasterPass);
    }
    if (sDirectRasterPass < 0)
    {
        return;
    }
    location = shader->getUniformLocation(viewport);
    if (location >= 0)
    {
        glProgramUniform2i(shader->mProgramObject, location,
                           sResources.viewportWidth, sResources.viewportHeight);
    }
    location = shader->getUniformLocation(volume_size);
    if (location >= 0)
    {
        glProgramUniform2i(shader->mProgramObject, location,
                           sResources.volumeWidth, sResources.volumeHeight);
    }
    location = shader->getUniformLocation(transmittance_sampler);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           directTransmittanceTextureUnit());
    }
}

void FSAVBOIT::finishDirectOccupancy()
{
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(pass, 1);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 3);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    beginDirectRasterPass(1);
}

void FSAVBOIT::finishDirectExtinction()
{
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(pass, 5);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    beginDirectRasterPass(2);
}

bool FSAVBOIT::directFrameReady()
{
    return sDirectFrameReady;
}

bool FSAVBOIT::finishDirectFrame(LLRenderTarget& screen)
{
    if (!sDirectFrameReady)
    {
        return false;
    }

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gGL.getTexUnit(directTransmittanceTextureUnit())->unbind(LLTexUnit::TT_TEXTURE_3D);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString debug_mode_uniform("avboitDebugMode");
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderAVBOITDebugMode", 0);
    const U32 groups_x = (sResources.viewportWidth + 15u) / 16u;
    const U32 groups_y = (sResources.viewportHeight + 15u) / 16u;

    glBindImageTexture(2, screen.getTexture(), 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA16F);
    gAVBOITResolveProgram.bind();
    gAVBOITResolveProgram.uniform2i(viewport, sResources.viewportWidth,
                                    sResources.viewportHeight);
    gAVBOITResolveProgram.uniform2i(volume_size, sResources.volumeWidth,
                                    sResources.volumeHeight);
    gAVBOITResolveProgram.uniform1i(pass, 7);
    gAVBOITResolveProgram.uniform1i(debug_mode_uniform, debug_mode);
    gAVBOITResolveProgram.bindTexture(
        LLShaderMgr::DEFERRED_DIFFUSE, &gAVBOITOpaqueTarget,
        false, LLTexUnit::TFO_POINT, 0);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
    gAVBOITResolveProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    sDirectRasterPass = -1;
    sDirectFrameReady = false;
    return true;
}

bool FSAVBOIT::composite(LLRenderTarget& screen, LLRenderTarget& opaque,
                         GLuint heads, GLuint nodes, U32 width, U32 height)
{
    if (requested() && !sResources.available)
    {
        allocateResources(width, height);
    }
    if (!available() || !heads || !nodes ||
        width != screen.getWidth() || height != screen.getHeight())
    {
        static bool fallback_logged = false;
        if (gSavedSettings.getBOOL("RenderAVBOIT") && !fallback_logged)
        {
            fallback_logged = true;
            LL_WARNS("AVBOIT") << "Requested AVBOIT is unavailable; using Exact OIT fallback"
                                << LL_ENDL;
        }
        return false;
    }

    static bool active_logged = false;
    if (!active_logged)
    {
        active_logged = true;
        LL_INFOS("AVBOIT") << "Using approximate AVBOIT resolve at "
                           << sResources.volumeWidth << "x" << sResources.volumeHeight
                           << "x" << AVBOIT_SLICES << LL_ENDL;
    }

    LL_PROFILE_GPU_ZONE("AVBOIT composite");
    glCopyImageSubData(screen.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       opaque.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       width, height, 1);

    const U32 zero = 0u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);

    glBindImageTexture(0, heads, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glBindImageTexture(3, sResources.extinction, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(4, sResources.transmittance, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32F);
    glBindImageTexture(5, sResources.classification, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(6, sResources.zeroTransmittanceDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(7, sResources.totalTransmittance, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R32F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, nodes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sResources.occupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sResources.warp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, sResources.tileOccupancy);

    static LLStaticHashedString pass_uniform("avboitPass");
    static LLStaticHashedString viewport_uniform("avboitViewport");
    static LLStaticHashedString volume_size_uniform("avboitVolumeSize");
    static LLStaticHashedString transmittance_uniform("avboitTransmittanceSampler");
    static LLStaticHashedString debug_mode_uniform("avboitDebugMode");
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderAVBOITDebugMode", 0);
    const U32 viewport_groups_x = (width + 15u) / 16u;
    const U32 viewport_groups_y = (height + 15u) / 16u;
    const U32 volume_groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 volume_groups_y = (sResources.volumeHeight + 15u) / 16u;

    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport_uniform, width, height);
    gAVBOITVolumeProgram.uniform2i(
        volume_size_uniform, sResources.volumeWidth, sResources.volumeHeight);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT occupancy");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 0);
        glDispatchCompute(viewport_groups_x, viewport_groups_y, 1u);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT adaptive warp");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 1);
        glDispatchCompute(1u, 1u, 1u);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT tiled occupancy");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 2);
        glDispatchCompute(viewport_groups_x, viewport_groups_y, 1u);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT sparse volume clear");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 3);
        glDispatchCompute(volume_groups_x, volume_groups_y, 1u);
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT extinction");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 4);
        glDispatchCompute(viewport_groups_x, viewport_groups_y, 1u);
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT sparse integrate");
        gAVBOITVolumeProgram.uniform1i(pass_uniform, 5);
        glDispatchCompute(volume_groups_x, volume_groups_y, 1u);
    }
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    {
        LL_PROFILE_GPU_ZONE("AVBOIT resolve");
        glBindImageTexture(2, screen.getTexture(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        gAVBOITResolveProgram.bind();
        gAVBOITResolveProgram.uniform2i(viewport_uniform, width, height);
        gAVBOITResolveProgram.uniform2i(
            volume_size_uniform, sResources.volumeWidth, sResources.volumeHeight);
        gAVBOITResolveProgram.uniform1i(pass_uniform, 6);
        gAVBOITResolveProgram.uniform1i(debug_mode_uniform, debug_mode);
        gAVBOITResolveProgram.bindTexture(
            LLShaderMgr::DEFERRED_DIFFUSE, &opaque, false, LLTexUnit::TFO_POINT, 0);
        gAVBOITResolveProgram.uniform1i(transmittance_uniform, 1);
        gGL.getTexUnit(1)->bindManual(LLTexUnit::TT_TEXTURE_3D, sResources.transmittance);
        glDispatchCompute(viewport_groups_x, viewport_groups_y, 1u);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE_3D);
        gAVBOITResolveProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        gAVBOITResolveProgram.unbind();
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
}
