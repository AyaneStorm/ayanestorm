/**
 * @file fsavboit.cpp
 * @brief Approximate adaptive voxel-based OIT resolve.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_set>

#include "fsavboit.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "lldrawpoolalpha.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvieweroctree.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
constexpr U32 AVBOIT_SCALE = 8;
constexpr U32 AVBOIT_SLICES = 128;
constexpr U32 AVBOIT_PACKED_SLICES = AVBOIT_SLICES / 4;
constexpr U32 AVBOIT_VIRTUAL_SLICES = 8192;
constexpr U32 AVBOIT_Z_BINS = 8192;
constexpr U32 AVBOIT_ZBIN_RMQ_LEVELS = 14;
constexpr U32 AVBOIT_ENTITY_MASK_WORDS = 8;

void allocateAccumulationTexture(GLuint& texture, GLenum format,
                                  U32 width, U32 height)
{
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void configureAccumulationBlend()
{
    for (GLuint attachment = 1; attachment <= 3; ++attachment)
    {
        glEnablei(GL_BLEND, attachment);
        glBlendEquationi(attachment, GL_FUNC_ADD);
        glBlendFunci(attachment, GL_ONE, GL_ONE);
    }
}

S32 directTransmittanceTextureUnit()
{
    return llmax(0, gGLManager.mNumTextureImageUnits - 1);
}

S32 directOpaqueDepthTextureUnit()
{
    return llmax(0, gGLManager.mNumTextureImageUnits - 2);
}

LLGLSLShader gAVBOITVolumeProgram;
LLGLSLShader gAVBOITResolveProgram;
LLGLSLShader gAVBOITEarlyDepthProgram;
LLGLSLShader gAVBOITBoundsProgram;
LLGLSLShader gAVBOITGLTFProgram;
LLGLSLShader gAVBOITAlphaProgram;
LLGLSLShader gAVBOITSkinnedAlphaProgram;
LLGLSLShader gAVBOITPBRAlphaProgram;
LLGLSLShader gAVBOITSkinnedPBRAlphaProgram;
LLGLSLShader gAVBOITFullbrightAlphaProgram;
LLGLSLShader gAVBOITSkinnedFullbrightAlphaProgram;
LLGLSLShader gAVBOITEmissiveProgram;
LLGLSLShader gAVBOITSkinnedEmissiveProgram;
LLGLSLShader gAVBOITPBRGlowProgram;
LLGLSLShader gAVBOITSkinnedPBRGlowProgram;
LLGLSLShader gAVBOITMaterialAlphaProgram[LLMaterial::SHADER_COUNT * 2];
LLRenderTarget gAVBOITOpaqueTarget;
LLRenderTarget gAVBOITPrepassTarget;

bool cloneCaptureShader(LLGLSLShader& destination, const LLGLSLShader& source,
                        const std::string& name, const char* terminal)
{
    destination.mName = name;
    destination.mFeatures = source.mFeatures;
    // Source programs expose their post-link draw features. Capture variants
    // must not attach an additional lighting fragment object at link time.
    destination.mFeatures.calculatesLighting = false;
    destination.mFeatures.hasLighting = false;
    destination.mDefines = source.mDefines;
    destination.mShaderFiles = source.mShaderFiles;
    if (std::strcmp(terminal, "deferred/avboitEmissiveF.glsl") == 0 ||
        std::strcmp(terminal, "deferred/avboitPbrGlowF.glsl") == 0)
    {
        destination.mShaderFiles.erase(
            std::remove_if(
                destination.mShaderFiles.begin(), destination.mShaderFiles.end(),
                [](const auto& shader_file)
                {
                    return shader_file.second == GL_FRAGMENT_SHADER;
                }),
            destination.mShaderFiles.end());
    }
    destination.mShaderFiles.emplace_back(terminal, GL_FRAGMENT_SHADER);
    destination.mShaderLevel = source.mShaderLevel;
    destination.mShaderGroup = source.mShaderGroup;
    destination.addPermutation("AVBOIT", "1");
    return destination.createShader();
}

bool cloneCapturePair(LLGLSLShader& destination, LLGLSLShader& rigged_destination,
                      const LLGLSLShader& source, const std::string& name,
                      const char* terminal)
{
    if (!source.mRiggedVariant ||
        !cloneCaptureShader(rigged_destination, *source.mRiggedVariant,
                            "Skinned " + name, terminal))
    {
        return false;
    }
    destination.mRiggedVariant = &rigged_destination;
    return cloneCaptureShader(destination, source, name, terminal);
}

void unloadMaterialShaders()
{
    gAVBOITGLTFProgram.unload();
    gAVBOITAlphaProgram.unload();
    gAVBOITSkinnedAlphaProgram.unload();
    gAVBOITPBRAlphaProgram.unload();
    gAVBOITSkinnedPBRAlphaProgram.unload();
    gAVBOITFullbrightAlphaProgram.unload();
    gAVBOITSkinnedFullbrightAlphaProgram.unload();
    gAVBOITEmissiveProgram.unload();
    gAVBOITSkinnedEmissiveProgram.unload();
    gAVBOITPBRGlowProgram.unload();
    gAVBOITSkinnedPBRGlowProgram.unload();
    for (LLGLSLShader& shader : gAVBOITMaterialAlphaProgram)
    {
        shader.unload();
    }
}
}

FSAVBOIT::Resources FSAVBOIT::sResources;
S32 FSAVBOIT::sDirectRasterPass = -1;
bool FSAVBOIT::sDirectFrameReady = false;
bool FSAVBOIT::sCaptureActive = false;
bool FSAVBOIT::sCaptureCompleted = false;

const char* FSAVBOIT::shaderCacheRevision()
{
    return "AVBOIT shader revision v89";
}

bool FSAVBOIT::supported()
{
    return gGLManager.mGLVersion >= 4.29f &&
        (gGLManager.mGLSLVersionMajor > 4 ||
         (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 30));
}

bool FSAVBOIT::requested()
{
    return gSavedSettings.getBOOL("RenderAVBOIT") && supported();
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

    gAVBOITEarlyDepthProgram.mName = "AVBOIT Early Depth";
    gAVBOITEarlyDepthProgram.mFeatures.attachNothing = true;
    gAVBOITEarlyDepthProgram.mShaderFiles.clear();
    gAVBOITEarlyDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitEarlyDepthV.glsl", GL_VERTEX_SHADER);
    gAVBOITEarlyDepthProgram.mShaderFiles.emplace_back(
        "deferred/avboitEarlyDepthF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITEarlyDepthProgram.mShaderLevel = shader_level;
    gAVBOITEarlyDepthProgram.clearPermutations();

    gAVBOITBoundsProgram.mName = "AVBOIT Conservative Bounds";
    gAVBOITBoundsProgram.mFeatures.attachNothing = true;
    gAVBOITBoundsProgram.mShaderFiles.clear();
    gAVBOITBoundsProgram.mShaderFiles.emplace_back(
        "deferred/avboitBoundsV.glsl", GL_VERTEX_SHADER);
    gAVBOITBoundsProgram.mShaderFiles.emplace_back(
        "deferred/avboitBoundsF.glsl", GL_FRAGMENT_SHADER);
    gAVBOITBoundsProgram.mShaderLevel = shader_level;
    gAVBOITBoundsProgram.clearPermutations();
    gAVBOITBoundsProgram.addPermutation("AVBOIT", "1");

    bool success = gAVBOITVolumeProgram.createShader() &&
        gAVBOITResolveProgram.createShader() &&
        gAVBOITEarlyDepthProgram.createShader() &&
        gAVBOITBoundsProgram.createShader();
    success = success && cloneCapturePair(
        gAVBOITAlphaProgram, gAVBOITSkinnedAlphaProgram,
        gDeferredAlphaProgram, "Deferred Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITPBRAlphaProgram, gAVBOITSkinnedPBRAlphaProgram,
        gDeferredPBRAlphaProgram, "Deferred PBR Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITFullbrightAlphaProgram, gAVBOITSkinnedFullbrightAlphaProgram,
        gDeferredFullbrightAlphaMaskAlphaProgram,
        "Deferred Fullbright Alpha AVBOIT Shader",
        "deferred/avboitCaptureF.glsl");
    success = success && cloneCapturePair(
        gAVBOITEmissiveProgram, gAVBOITSkinnedEmissiveProgram,
        gDeferredEmissiveProgram, "Deferred Emissive AVBOIT Shader",
        "deferred/avboitEmissiveF.glsl");
    success = success && cloneCapturePair(
        gAVBOITPBRGlowProgram, gAVBOITSkinnedPBRGlowProgram,
        gPBRGlowProgram, "PBR Glow AVBOIT Shader",
        "deferred/avboitPbrGlowF.glsl");

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2 && success; ++i)
    {
        if ((i & 0x3u) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            continue;
        }
        success = cloneCaptureShader(
            gAVBOITMaterialAlphaProgram[i], gDeferredMaterialProgram[i],
            llformat("Material AVBOIT Shader %u", i),
            "deferred/avboitCaptureF.glsl");
        if (i < LLMaterial::SHADER_COUNT)
        {
            gAVBOITMaterialAlphaProgram[i].mRiggedVariant =
                &gAVBOITMaterialAlphaProgram[i + LLMaterial::SHADER_COUNT];
        }
    }

    if (success)
    {
        gAVBOITGLTFProgram.mName = "AVBOIT GLTF PBR Metallic Roughness Shader";
        gAVBOITGLTFProgram.mFeatures = gGLTFPBRMetallicRoughnessProgram.mFeatures;
        gAVBOITGLTFProgram.mDefines = gGLTFPBRMetallicRoughnessProgram.mDefines;
        gAVBOITGLTFProgram.mShaderFiles =
            gGLTFPBRMetallicRoughnessProgram.mShaderFiles;
        gAVBOITGLTFProgram.mShaderFiles.emplace_back(
            "deferred/avboitCaptureF.glsl", GL_FRAGMENT_SHADER);
        gAVBOITGLTFProgram.mShaderLevel =
            gGLTFPBRMetallicRoughnessProgram.mShaderLevel;
        gAVBOITGLTFProgram.mShaderGroup =
            gGLTFPBRMetallicRoughnessProgram.mShaderGroup;
        gAVBOITGLTFProgram.addPermutation("AVBOIT", "1");
        gAVBOITGLTFProgram.mGLTFVariants.resize(
            gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size());
        for (U32 i = 0;
             i < gGLTFPBRMetallicRoughnessProgram.mGLTFVariants.size() && success;
             ++i)
        {
            success = cloneCaptureShader(
                gAVBOITGLTFProgram.mGLTFVariants[i],
                gGLTFPBRMetallicRoughnessProgram.mGLTFVariants[i],
                "AVBOIT GLTF PBR Metallic Roughness Variant",
                "deferred/avboitCaptureF.glsl");
        }
    }

    if (!success)
    {
        unloadShaders();
        LL_WARNS("AVBOIT") << "AVBOIT shaders unavailable; dispatcher fallback remains active"
                            << LL_ENDL;
    }
}

void FSAVBOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list)
{
    shader_list.push_back(&gAVBOITVolumeProgram);
    shader_list.push_back(&gAVBOITResolveProgram);
    shader_list.push_back(&gAVBOITEarlyDepthProgram);
    shader_list.push_back(&gAVBOITBoundsProgram);
    shader_list.push_back(&gAVBOITGLTFProgram);
    shader_list.push_back(&gAVBOITAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedAlphaProgram);
    shader_list.push_back(&gAVBOITPBRAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedPBRAlphaProgram);
    shader_list.push_back(&gAVBOITFullbrightAlphaProgram);
    shader_list.push_back(&gAVBOITSkinnedFullbrightAlphaProgram);
    shader_list.push_back(&gAVBOITEmissiveProgram);
    shader_list.push_back(&gAVBOITSkinnedEmissiveProgram);
    shader_list.push_back(&gAVBOITPBRGlowProgram);
    shader_list.push_back(&gAVBOITSkinnedPBRGlowProgram);
    for (U32 i = 0; i < LLMaterial::SHADER_COUNT; ++i)
    {
        if ((i & 0x3u) == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            shader_list.push_back(&gAVBOITMaterialAlphaProgram[i]);
        }
    }
}

void FSAVBOIT::unloadShaders()
{
    gAVBOITVolumeProgram.unload();
    gAVBOITResolveProgram.unload();
    gAVBOITEarlyDepthProgram.unload();
    gAVBOITBoundsProgram.unload();
    unloadMaterialShaders();
}

bool FSAVBOIT::shadersReady()
{
    return gAVBOITVolumeProgram.mProgramObject &&
        gAVBOITResolveProgram.mProgramObject &&
        gAVBOITEarlyDepthProgram.mProgramObject &&
        gAVBOITBoundsProgram.mProgramObject &&
        gAVBOITAlphaProgram.mProgramObject &&
        gAVBOITPBRAlphaProgram.mProgramObject &&
        gAVBOITFullbrightAlphaProgram.mProgramObject &&
        gAVBOITEmissiveProgram.mProgramObject &&
        gAVBOITPBRGlowProgram.mProgramObject;
}

void FSAVBOIT::beginFrame()
{
    // Mode-transition invalidation is centralized in the neutral dispatcher.
    sCaptureActive = false;
    sCaptureCompleted = false;
    sDirectRasterPass = -1;
    sDirectFrameReady = false;
}

bool FSAVBOIT::captureActive()
{
    return sCaptureActive;
}

bool FSAVBOIT::captureCompleted()
{
    return sCaptureCompleted;
}

bool FSAVBOIT::renderPostDeferredCapture(
    LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
    LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader)
{
    if (!requested() || !shadersReady() ||
        pool.getType() != LLDrawPool::POOL_ALPHA_POST_WATER ||
        LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender ||
        gCubeSnapshot || !beginDirectFrame(gPipeline.mRT->screen))
    {
        return false;
    }

    prepare(&gAVBOITAlphaProgram, true, water_sign);
    prepare(&gAVBOITPBRAlphaProgram, true, water_sign);
    prepare(&gAVBOITFullbrightAlphaProgram, true, water_sign);
    for (LLGLSLShader& shader : gAVBOITMaterialAlphaProgram)
    {
        if (shader.mProgramObject)
        {
            prepare(&shader, true, water_sign);
        }
    }
    prepare(&gAVBOITEmissiveProgram, false, water_sign);
    prepare(&gAVBOITPBRGlowProgram, false, water_sign);
    emissive_shader = emissiveShader();
    pbr_emissive_shader = pbrGlowShader();
    LLGLSLShader::unbind();

    const auto render_pass = [&pool]()
    {
        configureDirectRasterShader(&gAVBOITEmissiveProgram);
        configureDirectRasterShader(&gAVBOITSkinnedEmissiveProgram);
        configureDirectRasterShader(&gAVBOITPBRGlowProgram);
        configureDirectRasterShader(&gAVBOITSkinnedPBRGlowProgram);
        sCaptureActive = true;
        pool.forwardRender(true);
        pool.forwardRender(false);
        sCaptureActive = false;
    };

    gGL.setColorMask(false, false);
    {
        LL_PROFILE_GPU_ZONE("AVBOIT occupancy raster");
        rasterizeConservativeBounds();
        render_pass();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT depth warp and sparse clear");
        finishDirectOccupancy();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT extinction raster");
        render_pass();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT extinction integration");
        finishDirectExtinction();
    }
    {
        LL_PROFILE_GPU_ZONE("AVBOIT weighted color raster");
        render_pass();
    }
    finishDirectColorRaster();
    gGL.setColorMask(true, true);
    sCaptureCompleted = true;
    return true;
}

bool FSAVBOIT::configureCapturedDrawIfActive(LLGLSLShader* shader)
{
    if (!sCaptureActive)
    {
        return false;
    }
    if (shader)
    {
        static LLStaticHashedString glow("oitGlow");
        shader->uniform1f(glow, 0.f);
        configureDirectRasterShader(shader);
    }
    return true;
}

bool FSAVBOIT::handleCapturedEmissives(
    LLDrawPoolAlpha& pool, bool depth_only,
    std::vector<LLDrawInfo*>& emissives,
    std::vector<LLDrawInfo*>& pbr_emissives,
    std::vector<LLDrawInfo*>& rigged_emissives,
    std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    if (!sCaptureActive)
    {
        return false;
    }
    if (!depth_only)
    {
        if (sDirectRasterPass == 2)
        {
            configureAccumulationBlend();
        }
        if (!emissives.empty()) pool.renderEmissives(emissives);
        if (!pbr_emissives.empty()) pool.renderPbrEmissives(pbr_emissives);
        if (!rigged_emissives.empty()) pool.renderRiggedEmissives(rigged_emissives);
        if (!pbr_rigged_emissives.empty())
            pool.renderRiggedPbrEmissives(pbr_rigged_emissives);
    }
    return true;
}

void FSAVBOIT::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    static LLStaticHashedString glow("oitGlow");
    shader.uniform1f(glow, 0.f);
    configureDirectRasterShader(&shader);
}

bool FSAVBOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen)
{
    if (!directFrameReady() || !finishDirectFrame(screen))
    {
        return false;
    }
    for (LLDrawPool* pool : pipeline.mPools)
    {
        if (pool->getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
        {
            static_cast<LLDrawPoolAlpha*>(pool)->renderDebugAlpha();
            break;
        }
    }
    return true;
}

LLGLSLShader& FSAVBOIT::gltfProgram(LLGLSLShader& ordinary_program)
{
    return sCaptureActive ? gAVBOITGLTFProgram : ordinary_program;
}

LLGLSLShader* FSAVBOIT::alphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITPBRAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gAVBOITFullbrightAlphaProgram : ordinary;
}

LLGLSLShader* FSAVBOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary)
{
    LLGLSLShader& shader = gAVBOITMaterialAlphaProgram[mask];
    return sCaptureActive && shader.mProgramObject ? &shader : ordinary;
}

LLGLSLShader* FSAVBOIT::emissiveShader()
{
    return &gAVBOITEmissiveProgram;
}

LLGLSLShader* FSAVBOIT::pbrGlowShader()
{
    return &gAVBOITPBRGlowProgram;
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
                   sResources.volumeWidth, sResources.volumeHeight,
                   AVBOIT_PACKED_SLICES);

    glGenTextures(1, &sResources.transmittance);
    glBindTexture(GL_TEXTURE_3D, sResources.transmittance);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R8,
                   sResources.volumeWidth, sResources.volumeHeight, AVBOIT_SLICES);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

    glGenTextures(1, &sResources.zeroTransmittanceDepth);
    glBindTexture(GL_TEXTURE_2D, sResources.zeroTransmittanceDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI,
                   sResources.volumeWidth, sResources.volumeHeight);

    glGenTextures(1, &sResources.extinctionOverflowDepth);
    glBindTexture(GL_TEXTURE_2D, sResources.extinctionOverflowDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI,
                   sResources.volumeWidth, sResources.volumeHeight);

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

    const U32 tile_count =
        ((width + 15u) / 16u) * ((height + 15u) / 16u);
    const U64 work_words = 8u + AVBOIT_SLICES +
        static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight +
        static_cast<U64>(tile_count) * 4u +
        AVBOIT_Z_BINS * AVBOIT_ZBIN_RMQ_LEVELS +
        static_cast<U64>(sResources.volumeWidth) *
            sResources.volumeHeight * AVBOIT_ENTITY_MASK_WORDS +
        static_cast<U64>(sResources.volumeWidth) *
            sResources.volumeHeight * 5u;
    glGenBuffers(1, &sResources.work);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.work);
    glBufferData(GL_SHADER_STORAGE_BUFFER, work_words * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &sResources.diagnostics);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.diagnostics);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 8u * sizeof(U32),
                 nullptr, GL_DYNAMIC_DRAW);

    allocateAccumulationTexture(sResources.accumulatedColorGlow,
                                GL_RGBA16F, width, height);
    allocateAccumulationTexture(sResources.accumulatedWeight,
                                GL_R16F, width, height);
    allocateAccumulationTexture(sResources.accumulatedExtinction,
                                GL_R16F, width, height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return glGetError() == GL_NO_ERROR &&
        gAVBOITOpaqueTarget.allocate(width, height, GL_RGBA16F, true) &&
        gAVBOITPrepassTarget.allocate(
            sResources.volumeWidth, sResources.volumeHeight, GL_RGBA8);
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
    if (sResources.zeroTransmittanceDepth)
        glDeleteTextures(1, &sResources.zeroTransmittanceDepth);
    if (sResources.extinctionOverflowDepth)
        glDeleteTextures(1, &sResources.extinctionOverflowDepth);
    if (sResources.occupancy) glDeleteBuffers(1, &sResources.occupancy);
    if (sResources.warp) glDeleteBuffers(1, &sResources.warp);
    if (sResources.tileOccupancy) glDeleteBuffers(1, &sResources.tileOccupancy);
    if (sResources.work) glDeleteBuffers(1, &sResources.work);
    if (sResources.diagnostics) glDeleteBuffers(1, &sResources.diagnostics);
    if (sResources.accumulatedColorGlow)
        glDeleteTextures(1, &sResources.accumulatedColorGlow);
    if (sResources.accumulatedWeight)
        glDeleteTextures(1, &sResources.accumulatedWeight);
    if (sResources.accumulatedExtinction)
        glDeleteTextures(1, &sResources.accumulatedExtinction);
    gAVBOITOpaqueTarget.release();
    gAVBOITPrepassTarget.release();
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
    info["AVBOIT_PACKED_EXTINCTION_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
         AVBOIT_PACKED_SLICES * sizeof(U32)) / (1024ull * 1024ull));
    info["AVBOIT_TRANSMITTANCE_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.volumeWidth) * sResources.volumeHeight *
         AVBOIT_SLICES) / (1024ull * 1024ull));
    info["AVBOIT_DIRECT_RASTER"] = sDirectRasterPass >= 0 || sDirectFrameReady;
    info["AVBOIT_ACCUMULATION_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.viewportWidth) * sResources.viewportHeight *
         12ull) / (1024ull * 1024ull));
    info["AVBOIT_STATUS"] = !supported() ? "Unavailable: OpenGL 4.3 is required" :
        !available() ? "Unavailable or disabled; dispatcher fallback active" :
        "Available";
}

bool FSAVBOIT::beginDirectFrame(LLRenderTarget& screen)
{
    const U32 width = screen.getWidth();
    const U32 height = screen.getHeight();
    // screen shares deferredScreen's depth attachment but does not own the
    // texture name, so get the sampled depth from its actual owner.
    const GLuint opaque_depth = gPipeline.mRT ?
        gPipeline.mRT->deferredScreen.getDepth() : 0;
    if (requested() && (!sResources.available ||
        sResources.viewportWidth != width || sResources.viewportHeight != height))
    {
        const GLint previous_fbo = LLRenderTarget::sCurFBO;
        allocateResources(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
    }
    if (!available() || !sResources.accumulatedColorGlow ||
        !sResources.accumulatedWeight || !sResources.accumulatedExtinction ||
        !sResources.work ||
        !opaque_depth ||
        !gAVBOITOpaqueTarget.isComplete() ||
        !gAVBOITPrepassTarget.isComplete())
    {
        return false;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LL_INFOS("AVBOIT") << "Using independent direct-raster AVBOIT without fragment lists"
                            << LL_ENDL;
    }

    glCopyImageSubData(screen.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       gAVBOITOpaqueTarget.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       width, height, 1);
    glCopyImageSubData(opaque_depth, GL_TEXTURE_2D, 0, 0, 0, 0,
                       gAVBOITOpaqueTarget.getDepth(), GL_TEXTURE_2D,
                       0, 0, 0, 0, width, height, 1);

    const U32 zero = 0u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.occupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.tileOccupancy);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.diagnostics);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.work);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    const U32 draw_command[4] = { 6u, 0u, 0u, 0u };
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 4u * sizeof(U32),
                    sizeof(draw_command), draw_command);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glBindImageTexture(3, sResources.extinction, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(4, sResources.transmittance, 0, GL_TRUE, 0,
                       GL_READ_WRITE, GL_R8);
    glBindImageTexture(6, sResources.zeroTransmittanceDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R8UI);
    glBindImageTexture(7, sResources.extinctionOverflowDepth, 0, GL_FALSE, 0,
                       GL_READ_WRITE, GL_R32UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sResources.occupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sResources.warp);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, sResources.tileOccupancy);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, sResources.diagnostics);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sResources.work);
    gGL.getTexUnit(directOpaqueDepthTextureUnit())->bindManual(
        LLTexUnit::TT_TEXTURE, opaque_depth);

    // Full-resolution occupancy is conservatively folded into 8x8 cells.
    // Use the private opaque-depth target so thin final-raster coverage cannot
    // disappear merely because it missed a low-resolution sample center.
    gAVBOITOpaqueTarget.bindTarget();
    sDirectFrameReady = false;
    beginDirectRasterPass(0);
    return true;
}

void FSAVBOIT::beginDirectRasterPass(S32 pass)
{
    sDirectRasterPass = pass;
    if (pass == 0)
    {
        glViewport(0, 0, sResources.viewportWidth,
                   sResources.viewportHeight);
    }
    else if (pass == 1)
    {
        // The reference AVBOIT extinction prepass rasterizes directly at
        // one-eighth resolution; it does not fold 64 full-resolution samples.
        glViewport(0, 0, sResources.volumeWidth, sResources.volumeHeight);
    }
    if (pass == 2)
    {
        glViewport(0, 0, sResources.viewportWidth, sResources.viewportHeight);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D,
                               sResources.accumulatedColorGlow, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                               GL_TEXTURE_2D,
                               sResources.accumulatedWeight, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3,
                               GL_TEXTURE_2D,
                               sResources.accumulatedExtinction, 0);
        const GLenum draw_buffers[] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
        };
        glDrawBuffers(4, draw_buffers);
        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        for (GLuint attachment = 1; attachment <= 3; ++attachment)
        {
            glColorMaski(attachment, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnablei(GL_BLEND, attachment);
            glBlendEquationi(attachment, GL_FUNC_ADD);
            glBlendFunci(attachment, GL_ONE, GL_ONE);
        }
        const GLfloat clear_value[4] = { 0.f, 0.f, 0.f, 0.f };
        glClearBufferfv(GL_COLOR, 1, clear_value);
        glClearBufferfv(GL_COLOR, 2, clear_value);
        glClearBufferfv(GL_COLOR, 3, clear_value);
        gGL.getTexUnit(directTransmittanceTextureUnit())->bindManual(
            LLTexUnit::TT_TEXTURE_3D, sResources.transmittance);
        sDirectFrameReady = true;
    }
}

void FSAVBOIT::rasterizeConservativeBounds()
{
    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString depth_range("avboitDepthRange");
    static LLStaticHashedString opaque_depth_sampler(
        "avboitOpaqueDepthSampler");
    static LLStaticHashedString entity_id_uniform("avboitEntityID");
    static LLStaticHashedString proxy_depth_interval(
        "avboitProxyDepthInterval");
    static LLStaticHashedString exact_proxy("avboitExactProxy");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    // Initialize the interval words with compute so the portable GL 4.3
    // baseline retains an explicit empty sentinel.
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(pass, 9);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    gAVBOITBoundsProgram.bind();
    gAVBOITBoundsProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITBoundsProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    LLCamera* camera = LLViewerCamera::getInstance();
    gAVBOITBoundsProgram.uniform2f(depth_range, camera->getNear(),
                                   camera->getFar());
    gAVBOITBoundsProgram.uniform1i(
        opaque_depth_sampler, directOpaqueDepthTextureUnit());
    gAVBOITBoundsProgram.uniform1i(exact_proxy, 0);
    // AABB centers are in agent space. Clear any model matrix left by the
    // preceding scene draw so their projection matches CPU camera depths.
    LLRenderPass::applyModelMatrix(nullptr);
    gPipeline.mCubeVB->setBuffer();

    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    struct BoundRecord
    {
        LLVector3 center;
        LLVector3 size;
        F32 minimumDepth = 0.f;
        F32 maximumDepth = 0.f;
    };
    std::vector<BoundRecord> bounds;
    std::unordered_set<LLSpatialGroup*> gathered_groups;
    const F32 water_height = LLEnvironment::instance().getWaterHeight();
    const bool above_water = !LLPipeline::sUnderWaterRender;
    const LLVector3 camera_origin = camera->getOrigin();
    const LLVector3 camera_at = camera->getAtAxis();
    const auto gather_group_range =
        [&bounds, &gathered_groups, camera_origin, camera_at,
         water_height, above_water](
            LLCullResult::sg_iterator begin,
            LLCullResult::sg_iterator end,
            U32 draw_pass)
    {
        for (LLCullResult::sg_iterator iter = begin; iter != end; ++iter)
        {
            LLSpatialGroup* group = *iter;
            if (!group || group->isDead() ||
                !group->getSpatialPartition()->mRenderByGroup)
            {
                continue;
            }
            const auto draw_entries = group->mDrawMap.find(draw_pass);
            if (draw_entries == group->mDrawMap.end() ||
                draw_entries->second.empty() ||
                !gathered_groups.insert(group).second)
            {
                continue;
            }

            LLSpatialBridge* bridge =
                group->getSpatialPartition()->asBridge();
            const LLVector4a* extents =
                bridge ? bridge->getSpatialExtents() : group->getExtents();
            const U32 partition_type =
                group->getSpatialPartition()->mPartitionType;
            const bool particle =
                partition_type == LLViewerRegion::PARTITION_PARTICLE ||
                partition_type == LLViewerRegion::PARTITION_HUD_PARTICLE;
            if ((!gPipeline.sRenderParticles && particle) ||
                (above_water &&
                 extents[1].getF32ptr()[2] < water_height) ||
                (!above_water &&
                 extents[0].getF32ptr()[2] > water_height))
            {
                continue;
            }
            LLVector4a center;
            LLVector4a size;
            center.setAdd(extents[0], extents[1]);
            center.mul(0.5f);
            size.setSub(extents[1], extents[0]);
            size.mul(0.5f);

            BoundRecord record;
            record.center = LLVector3(center.getF32ptr());
            record.size = LLVector3(size.getF32ptr());
            constexpr F32 proxy_fudge = 0.25f;
            record.size.mV[0] += proxy_fudge;
            record.size.mV[1] += proxy_fudge;
            record.size.mV[2] += proxy_fudge;
            const F32 center_depth =
                (record.center - camera_origin) * camera_at;
            const F32 depth_radius =
                fabsf(camera_at.mV[0]) * record.size.mV[0] +
                fabsf(camera_at.mV[1]) * record.size.mV[1] +
                fabsf(camera_at.mV[2]) * record.size.mV[2];
            record.minimumDepth = center_depth - depth_radius;
            record.maximumDepth = center_depth + depth_radius;
            bounds.push_back(record);
        }
    };

    gather_group_range(gPipeline.beginAlphaGroups(),
                       gPipeline.endAlphaGroups(), LLRenderPass::PASS_ALPHA);
    gather_group_range(gPipeline.beginRiggedAlphaGroups(),
                       gPipeline.endRiggedAlphaGroups(),
                       LLRenderPass::PASS_ALPHA_RIGGED);
    std::sort(bounds.begin(), bounds.end(),
              [](const BoundRecord& left, const BoundRecord& right)
              {
                  return left.minimumDepth < right.minimumDepth ||
                      (left.minimumDepth == right.minimumDepth &&
                       left.maximumDepth < right.maximumDepth);
              });

    // DRO17 CPU Z bins: bounds are ordered by conservative near depth and
    // every uniform bin stores a packed 16-bit minimum/maximum entity ID.
    // ID 65534 is the conservative overflow bucket; 0xffff marks no entity.
    std::vector<U16> zbin_min(AVBOIT_Z_BINS, 0xffffu);
    std::vector<U16> zbin_max(AVBOIT_Z_BINS, 0u);
    const F32 near_depth = camera->getNear();
    const F32 far_depth = camera->getFar();
    const F32 depth_range_value =
        llmax(far_depth - near_depth, 0.0001f);
    std::vector<U32> end_order(bounds.size());
    for (U32 index = 0; index < end_order.size(); ++index)
    {
        end_order[index] = index;
    }
    std::sort(end_order.begin(), end_order.end(),
              [&bounds](U32 left, U32 right)
              {
                  return bounds[left].maximumDepth <
                      bounds[right].maximumDepth;
              });
    std::multiset<U16> active_ids;
    U32 start_cursor = 0u;
    U32 end_cursor = 0u;
    for (U32 bin = 0; bin < AVBOIT_Z_BINS; ++bin)
    {
        const F32 bin_min = near_depth +
            depth_range_value * (F32(bin) / F32(AVBOIT_Z_BINS));
        const F32 bin_max = near_depth +
            depth_range_value * (F32(bin + 1u) / F32(AVBOIT_Z_BINS));
        while (start_cursor < bounds.size() &&
               bounds[start_cursor].minimumDepth <= bin_max)
        {
            active_ids.insert(
                U16(llmin(start_cursor, U32(0xfffeu))));
            ++start_cursor;
        }
        while (end_cursor < end_order.size() &&
               bounds[end_order[end_cursor]].maximumDepth < bin_min)
        {
            const U16 entity_id =
                U16(llmin(end_order[end_cursor], U32(0xfffeu)));
            const auto active = active_ids.find(entity_id);
            if (active != active_ids.end())
            {
                active_ids.erase(active);
            }
            ++end_cursor;
        }
        if (!active_ids.empty())
        {
            zbin_min[bin] = *active_ids.begin();
            zbin_max[bin] = *active_ids.rbegin();
        }
    }
    std::vector<U32> packed_zbins(AVBOIT_Z_BINS, 0xffffffffu);
    for (U32 bin = 0; bin < AVBOIT_Z_BINS; ++bin)
    {
        if (zbin_min[bin] != 0xffffu)
        {
            packed_zbins[bin] =
                U32(zbin_min[bin]) | (U32(zbin_max[bin]) << 16u);
        }
    }
    // Sparse-table range minima/maxima let the GL 4.3 compute path query all
    // uniform Z bins intersecting a cell interval with two vector loads.
    std::vector<U32> zbin_ranges(
        AVBOIT_Z_BINS * AVBOIT_ZBIN_RMQ_LEVELS, 0xffffffffu);
    std::copy(packed_zbins.begin(), packed_zbins.end(),
              zbin_ranges.begin());
    for (U32 level = 1u; level < AVBOIT_ZBIN_RMQ_LEVELS; ++level)
    {
        const U32 half_span = 1u << (level - 1u);
        const U32 previous = (level - 1u) * AVBOIT_Z_BINS;
        const U32 destination = level * AVBOIT_Z_BINS;
        for (U32 bin = 0u; bin < AVBOIT_Z_BINS; ++bin)
        {
            const U32 left = zbin_ranges[previous + bin];
            const U32 right_index = bin + half_span;
            const U32 right = right_index < AVBOIT_Z_BINS ?
                zbin_ranges[previous + right_index] : 0xffffffffu;
            U32 minimum_id = left & 0xffffu;
            U32 maximum_id =
                minimum_id != 0xffffu ? left >> 16u : 0u;
            if ((right & 0xffffu) != 0xffffu)
            {
                minimum_id = minimum_id == 0xffffu ?
                    (right & 0xffffu) :
                    llmin(minimum_id, right & 0xffffu);
                maximum_id = llmax(maximum_id, right >> 16u);
            }
            if (minimum_id != 0xffffu)
            {
                zbin_ranges[destination + bin] =
                    minimum_id | (maximum_id << 16u);
            }
        }
    }
    const U32 tile_count =
        ((sResources.viewportWidth + 15u) / 16u) *
        ((sResources.viewportHeight + 15u) / 16u);
    const U64 zbin_offset_words = 8u + AVBOIT_SLICES +
        static_cast<U64>(sResources.volumeWidth) *
            sResources.volumeHeight +
        static_cast<U64>(tile_count) * 4u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.work);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                    zbin_offset_words * sizeof(U32),
                    zbin_ranges.size() * sizeof(U32),
                    zbin_ranges.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);

    for (U32 index = 0; index < bounds.size(); ++index)
    {
        const BoundRecord& record = bounds[index];
        gAVBOITBoundsProgram.uniform1i(
            entity_id_uniform, S32(llmin(index, U32(0xfffeu))));
        gAVBOITBoundsProgram.uniform2f(
            proxy_depth_interval,
            llmax(record.minimumDepth, near_depth),
            llmin(record.maximumDepth, far_depth));
        gAVBOITBoundsProgram.uniform3fv(
            LLShaderMgr::BOX_CENTER, 1, record.center.mV);
        gAVBOITBoundsProgram.uniform3fv(
            LLShaderMgr::BOX_SIZE, 1, record.size.mV);
        LLVector4a center;
        center.load3(record.center.mV);
        const U32 near_fan = get_box_fan_indices(camera, center);
        const U32 far_fan = near_fan ^ (7u * 8u);
        gPipeline.mCubeVB->drawRange(
            LLRender::TRIANGLE_FAN, 0, 7, 8, near_fan);
        gPipeline.mCubeVB->drawRange(
            LLRender::TRIANGLE_FAN, 0, 7, 8, far_fan);
    }

    // Static draw geometry supplies a coordinate-exact conservative proxy.
    // It intentionally ignores texture alpha, materials, and lighting, so
    // every actual alpha-tested fragment remains covered. Group AABBs stay
    // active for rigged geometry and as a coarse spatial fallback.
    gAVBOITBoundsProgram.uniform1i(exact_proxy, 1);
    gAVBOITBoundsProgram.uniform1i(entity_id_uniform, 0);
    gAVBOITBoundsProgram.uniform3f(
        LLShaderMgr::BOX_CENTER, 0.f, 0.f, 0.f);
    gAVBOITBoundsProgram.uniform3f(
        LLShaderMgr::BOX_SIZE, 1.f, 1.f, 1.f);
    for (LLCullResult::sg_iterator iter = gPipeline.beginAlphaGroups();
         iter != gPipeline.endAlphaGroups(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (!group || group->isDead() ||
            !group->getSpatialPartition()->mRenderByGroup)
        {
            continue;
        }
        const U32 partition_type =
            group->getSpatialPartition()->mPartitionType;
        const bool particle =
            partition_type == LLViewerRegion::PARTITION_PARTICLE ||
            partition_type == LLViewerRegion::PARTITION_HUD_PARTICLE;
        if (!gPipeline.sRenderParticles && particle)
        {
            continue;
        }
        const auto found =
            group->mDrawMap.find(LLRenderPass::PASS_ALPHA);
        if (found == group->mDrawMap.end())
        {
            continue;
        }
        for (LLPointer<LLDrawInfo>& draw : found->second)
        {
            if (draw.isNull() || draw->mAvatar != nullptr ||
                draw->mVertexBuffer.isNull())
            {
                continue;
            }
            LLRenderPass::applyModelMatrix(*draw);
            draw->mVertexBuffer->setBuffer();
            draw->mVertexBuffer->drawRange(
                LLRender::TRIANGLES, draw->mStart, draw->mEnd,
                draw->mCount, draw->mOffset);
        }
    }
    gAVBOITBoundsProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // A box crossing the near plane is not guaranteed to leave a closed
    // fixed-function proxy footprint. Conservatively seed every cell for
    // those rare camera-intersecting bounds before spatial dilation.
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform2f(depth_range, camera->getNear(),
                                   camera->getFar());
    for (U32 index = 0; index < bounds.size(); ++index)
    {
        const BoundRecord& record = bounds[index];
        if (record.minimumDepth <= near_depth &&
            record.maximumDepth >= near_depth)
        {
            gAVBOITVolumeProgram.uniform2f(
                proxy_depth_interval, near_depth,
                llmin(record.maximumDepth, far_depth));
            gAVBOITVolumeProgram.uniform1i(
                entity_id_uniform, S32(llmin(index, U32(0xfffeu))));
            gAVBOITVolumeProgram.uniform1i(pass, 10);
            glDispatchCompute(groups_x, groups_y, 1u);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
    // Dilate raw proxy intervals before material occupancy compares against
    // them. This pass depends only on the completed bounds raster.
    gAVBOITVolumeProgram.uniform1i(pass, 8);
    glDispatchCompute(groups_x, groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Return the material occupancy pass to the private opaque-depth target.
    gAVBOITOpaqueTarget.bindTarget();
    glViewport(0, 0, sResources.viewportWidth, sResources.viewportHeight);
}

void FSAVBOIT::finishDirectColorRaster()
{
    for (GLuint attachment = 1; attachment <= 3; ++attachment)
    {
        glDisablei(GL_BLEND, attachment);
        glColorMaski(attachment, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachment,
            GL_TEXTURE_2D, 0, 0);
    }
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT |
                    GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gAVBOITOpaqueTarget.flush();
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
    static LLStaticHashedString opaque_depth_sampler("avboitOpaqueDepthSampler");
    static LLStaticHashedString depth_range("avboitDepthRange");
    GLint location = shader->getUniformLocation(raster_pass);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location, sDirectRasterPass);
    }
    if (sDirectRasterPass < 0)
    {
        return;
    }
    if (sDirectRasterPass == 2)
    {
        // LLDrawPoolAlpha disables ordinary framebuffer blending for OIT
        // capture, so restore independent additive blending at draw time.
        configureAccumulationBlend();
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
    location = shader->getUniformLocation(opaque_depth_sampler);
    if (location >= 0)
    {
        glProgramUniform1i(shader->mProgramObject, location,
                           directOpaqueDepthTextureUnit());
    }
    location = shader->getUniformLocation(depth_range);
    if (location >= 0)
    {
        const LLCamera& camera = *LLViewerCamera::getInstance();
        glProgramUniform2f(shader->mProgramObject, location,
                           camera.getNear(), camera.getFar());
    }
}

void FSAVBOIT::finishDirectOccupancy()
{
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString depth_range("avboitDepthRange");
    const U32 groups_x = (sResources.volumeWidth + 15u) / 16u;
    const U32 groups_y = (sResources.volumeHeight + 15u) / 16u;

    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    const LLCamera& camera = *LLViewerCamera::getInstance();
    gAVBOITVolumeProgram.uniform2f(depth_range, camera.getNear(),
                                   camera.getFar());
    gAVBOITVolumeProgram.uniform1i(pass, 1);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 2);
    glDispatchCompute(groups_x, groups_y, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 4);
    glDispatchCompute(1u, 1u, 1u);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 3);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.work);
    glDispatchComputeIndirect(0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gAVBOITOpaqueTarget.flush();
    gAVBOITPrepassTarget.bindTarget();
    beginDirectRasterPass(1);
}

void FSAVBOIT::finishDirectExtinction()
{
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    const U32 tile_groups_x =
        ((sResources.viewportWidth + 15u) / 16u + 15u) / 16u;
    const U32 tile_groups_y =
        ((sResources.viewportHeight + 15u) / 16u + 15u) / 16u;
    gAVBOITVolumeProgram.bind();
    gAVBOITVolumeProgram.uniform2i(viewport, sResources.viewportWidth,
                                   sResources.viewportHeight);
    gAVBOITVolumeProgram.uniform2i(volume_size, sResources.volumeWidth,
                                   sResources.volumeHeight);
    gAVBOITVolumeProgram.uniform1i(pass, 5);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.work);
    glDispatchComputeIndirect(0);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT);
    gAVBOITVolumeProgram.uniform1i(pass, 6);
    glDispatchCompute(tile_groups_x, tile_groups_y, 1u);
    gAVBOITVolumeProgram.unbind();
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT);
    gAVBOITPrepassTarget.flush();

    // Rasterize conservative zero-transmittance quads into a private copy of
    // opaque depth. The final color pass then receives ordinary early-Z/Hi-Z
    // rejection without modifying the viewer's shared scene depth texture.
    gAVBOITOpaqueTarget.bindTarget();
    {
        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_LEQUAL);
        gAVBOITEarlyDepthProgram.bind();
        gAVBOITEarlyDepthProgram.uniform2i(
            viewport, sResources.viewportWidth, sResources.viewportHeight);
        gAVBOITEarlyDepthProgram.uniform2i(
            volume_size, sResources.volumeWidth, sResources.volumeHeight);
        gPipeline.mScreenTriangleVB->setBuffer();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sResources.work);
        glDrawArraysIndirect(
            GL_TRIANGLES,
            reinterpret_cast<const void*>(4u * sizeof(U32)));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        gAVBOITEarlyDepthProgram.unbind();
    }
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
    gGL.getTexUnit(directOpaqueDepthTextureUnit())->unbind(LLTexUnit::TT_TEXTURE);

    static LLStaticHashedString pass("avboitPass");
    static LLStaticHashedString viewport("avboitViewport");
    static LLStaticHashedString volume_size("avboitVolumeSize");
    static LLStaticHashedString debug_mode_uniform("avboitDebugMode");
    const S32 debug_mode =
        llclamp(gSavedSettings.getS32("RenderAVBOITDebugMode"), 0, 6);
    static S32 previous_debug_mode = -1;
    if (debug_mode != previous_debug_mode)
    {
        LL_INFOS("AVBOIT") << "AVBOIT diagnostic mode " << debug_mode
                            << LL_ENDL;
        previous_debug_mode = debug_mode;
    }
    const U32 groups_x = (sResources.viewportWidth + 15u) / 16u;
    const U32 groups_y = (sResources.viewportHeight + 15u) / 16u;

    glBindImageTexture(2, screen.getTexture(), 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(0, sResources.accumulatedColorGlow, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, sResources.accumulatedWeight, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_R16F);
    glBindImageTexture(5, sResources.accumulatedExtinction, 0, GL_FALSE, 0,
                       GL_READ_ONLY, GL_R16F);
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
