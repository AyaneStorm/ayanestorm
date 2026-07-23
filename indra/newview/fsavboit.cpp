/**
 * @file fsavboit.cpp
 * @brief Approximate adaptive voxel-based OIT resolve.
 * @author chanayane@firestorm
 */

#include "llviewerprecompiledheaders.h"

#include <algorithm>
#include <cstring>

#include "fsavboit.h"

#include "llglslshader.h"
#include "lldrawpoolalpha.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

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
    return "AVBOIT shader revision v43";
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

    bool success = gAVBOITVolumeProgram.createShader() &&
        gAVBOITResolveProgram.createShader();
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
    unloadMaterialShaders();
}

bool FSAVBOIT::shadersReady()
{
    return gAVBOITVolumeProgram.mProgramObject &&
        gAVBOITResolveProgram.mProgramObject &&
        gAVBOITAlphaProgram.mProgramObject &&
        gAVBOITPBRAlphaProgram.mProgramObject &&
        gAVBOITFullbrightAlphaProgram.mProgramObject &&
        gAVBOITEmissiveProgram.mProgramObject &&
        gAVBOITPBRGlowProgram.mProgramObject;
}

void FSAVBOIT::beginFrame()
{
    // AVBOIT, like Exact OIT, does not maintain vanilla's cached
    // within-group alpha ordering while it replaces the transparency pass.
    // Rebuild those groups when AVBOIT is disabled live.
    if (sCaptureCompleted && !requested())
    {
        const auto invalidate_alpha_groups =
            [](LLCullResult::sg_iterator begin, LLCullResult::sg_iterator end)
        {
            for (LLCullResult::sg_iterator iter = begin; iter != end; ++iter)
            {
                LLSpatialGroup* group = *iter;
                if (group && !group->isDead())
                {
                    group->setState(LLSpatialGroup::ALPHA_DIRTY);
                    gPipeline.markRebuild(group);
                }
            }
        };
        invalidate_alpha_groups(
            gPipeline.beginAlphaGroups(), gPipeline.endAlphaGroups());
        invalidate_alpha_groups(
            gPipeline.beginRiggedAlphaGroups(), gPipeline.endRiggedAlphaGroups());
    }
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

    LL_PROFILE_GPU_ZONE("Direct AVBOIT raster passes");
    gGL.setColorMask(false, false);
    render_pass();
    finishDirectOccupancy();
    render_pass();
    finishDirectExtinction();
    render_pass();
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
        !available() ? "Unavailable or disabled; dispatcher fallback active" :
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
        LL_INFOS("AVBOIT") << "Using independent direct-raster AVBOIT without fragment lists"
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
