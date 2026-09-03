/**
 * @file fsexactoit.cpp
 * @brief Firestorm Exact OIT implementation.
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsexactoit.h"

// Exact OIT depends on GLSL 4.30 compute shaders and SSBOs that macOS's
// capped OpenGL 4.1 does not provide. isSupported() already refuses at
// runtime there, but compile it out entirely on Darwin so the dead compute
// paths never end up in the Mac binary. Every call site (pipeline.cpp,
// llviewershadermgr.cpp, etc.) keeps linking against these inert stubs.
#if LL_DARWIN

#include "llrendertarget.h"

bool FSExactOIT::sCaptureCompleted = false;
bool FSExactOIT::sCaptureClearNeeded = false;
bool FSExactOIT::sVanillaFallbackActive = false;
bool FSExactOIT::sCaptureActive = false;
bool FSExactOIT::sRuntimeAllocationAttempted = false;
bool FSExactOIT::sRetainNodePoolOnRelease = false;
LLRenderTarget FSExactOIT::sOpaqueTarget;
FSExactOIT::Resources FSExactOIT::sResources;

const char* FSExactOIT::shaderCacheRevision() { return "exact-oit-unsupported"; }
bool FSExactOIT::isSupported() { return false; }
bool FSExactOIT::isEnabled() { return false; }
bool FSExactOIT::loadShaders(bool success, S32 shader_level, bool use_sun_shadow,
                             bool gltf_enabled, std::vector<LLGLSLShader*>& shader_list)
{
    return success;
}
void FSExactOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list) {}
void FSExactOIT::unloadShaders() {}
void FSExactOIT::appendDiagnostics(LLSD& info) {}
void FSExactOIT::beginFrame() {}
bool FSExactOIT::captureCompleted() { return false; }
bool FSExactOIT::captureActive() { return false; }
bool FSExactOIT::renderPostDeferredCapture(LLDrawPoolAlpha& pool, PrepareShader prepare,
                                           F32 water_sign, LLGLSLShader*& emissive_shader,
                                           LLGLSLShader*& pbr_emissive_shader)
{
    return false;
}
void FSExactOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen,
                             LLVertexBuffer& screen_triangle, bool cube_snapshot,
                             bool impostor_render, bool mouselook)
{
}
bool FSExactOIT::configureCapturedDrawIfActive(LLGLSLShader* shader, U32 color_source,
                                               U32 color_destination, U32 alpha_source,
                                               U32 alpha_destination)
{
    return false;
}
bool FSExactOIT::handleCapturedEmissives(LLDrawPoolAlpha& pool, bool depth_only,
                                         std::vector<LLDrawInfo*>& emissives,
                                         std::vector<LLDrawInfo*>& pbr_emissives,
                                         std::vector<LLDrawInfo*>& rigged_emissives,
                                         std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    return false;
}
void FSExactOIT::configureGLTFCapturedDraw(LLGLSLShader& shader) {}
LLGLSLShader& FSExactOIT::gltfProgram(LLGLSLShader& ordinary_program) { return ordinary_program; }
LLGLSLShader* FSExactOIT::alphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSExactOIT::pbrAlphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSExactOIT::fullbrightAlphaShader(LLGLSLShader* ordinary) { return ordinary; }
LLGLSLShader* FSExactOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary) { return ordinary; }
void FSExactOIT::retainNodePoolOnNextRelease() {}
void FSExactOIT::releaseResources() {}
void FSExactOIT::allocateResources(U32 width, U32 height) {}

#else // !LL_DARWIN

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "asbackgroundisolate.h"
#include "llgl.h"
#include "lldrawpoolalpha.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsd.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
// Adds viewer-wide permutations shared by all Exact OIT shader programs.
void addCommonPermutations(LLGLSLShader& shader)
{
    static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);

    if (emissive)
    {
        shader.addPermutation("HAS_EMISSIVE", "1");
    }
}

// Links the shared node-capture fragment implementation into a shader program.
void addCaptureFragment(LLGLSLShader& shader)
{
    shader.mShaderFiles.emplace_back("deferred/exactOITCaptureF.glsl", GL_FRAGMENT_SHADER);
}

// Adds the EXACT_OIT permutation and, when the setting requests it, the
// no-op discard permutation. Call once per capture shader alongside its
// other EXACT_OIT-family permutations.
void addExactOITPermutations(LLGLSLShader& shader)
{
    static LLCachedControl<bool> discard_no_op(gSavedSettings, "RenderExactOITNoOpCapture", true);
    shader.addPermutation("EXACT_OIT", "1");
    if (discard_no_op)
    {
        shader.addPermutation("EXACT_OIT_DISCARD_NOOP", "1");
    }
}

// Per-program cache of the packed-blend-factor uniform: avoids a hashed
// lookup and a redundant re-upload on every capture draw. Keyed by program
// pointer (GL uniform state is per program object and survives rebinding);
// cleared whenever programs are relinked.
struct BlendUniformCache { GLint location = -2; U32 value = 0xffffffffu; };
std::unordered_map<const LLGLSLShader*, BlendUniformCache> sBlendCache;

void uploadBlendFactors(LLGLSLShader& shader, U32 packed)
{
    BlendUniformCache& cache = sBlendCache[&shader];
    if (cache.location == -2)
    {
        static LLStaticHashedString name("oitBlendFactors");
        cache.location = shader.getUniformLocation(name);
    }
    // NOTE: do not route this through LLGLSLShader::uniform1i(); its value
    // cache stores the value as F32, and packed blend tuples exceed the
    // 24-bit exact integer range, so two different tuples can compare equal
    // and the upload gets silently skipped.
    if (cache.location >= 0 && cache.value != packed)
    {
        glUniform1ui(cache.location, packed);
        cache.value = packed;
    }
}

// Creates the skinned counterpart of a shader and returns whether compilation succeeded.
bool makeRiggedVariant(LLGLSLShader& shader, LLGLSLShader& rigged_shader)
{
    rigged_shader.mName = llformat("Skinned %s", shader.mName.c_str());
    rigged_shader.mFeatures = shader.mFeatures;
    rigged_shader.mFeatures.hasObjectSkinning = true;
    rigged_shader.mDefines = shader.mDefines;
    rigged_shader.addPermutation("HAS_SKIN", "1");
    rigged_shader.mShaderFiles = shader.mShaderFiles;
    rigged_shader.mShaderLevel = shader.mShaderLevel;
    rigged_shader.mShaderGroup = shader.mShaderGroup;

    shader.mRiggedVariant = &rigged_shader;
    return rigged_shader.createShader();
}

// Configures and compiles one GLTF feature permutation; returns its creation result.
bool makeGLTFVariant(LLGLSLShader& shader, LLGLSLShader& variant, bool alpha_blend,
                     bool rigged, bool unlit, bool multi_uv, bool use_sun_shadow)
{
    variant.mName = shader.mName;
    variant.mFeatures = shader.mFeatures;
    variant.mShaderFiles = shader.mShaderFiles;
    variant.mShaderLevel = shader.mShaderLevel;
    variant.mShaderGroup = shader.mShaderGroup;
    variant.mDefines = shader.mDefines;

    constexpr U32 node_size = 16 * 3;
    const U32 max_nodes = gGLManager.mMaxUniformBlockSize / node_size;
    variant.addPermutation("MAX_NODES_PER_GLTF_OBJECT", std::to_string(max_nodes));

    constexpr U32 material_size = 16 * 12;
    const U32 max_materials = gGLManager.mMaxUniformBlockSize / material_size;
    LLGLSLShader::sMaxGLTFMaterials = max_materials;
    variant.addPermutation("MAX_MATERIALS_PER_GLTF_OBJECT", std::to_string(max_materials));

    const U32 max_vec4s = gGLManager.mMaxUniformBlockSize / 16;
    variant.addPermutation("MAX_UBO_VEC4S", std::to_string(max_vec4s));

    if (rigged)
    {
        variant.addPermutation("HAS_SKIN", "1");
    }
    if (unlit)
    {
        variant.addPermutation("UNLIT", "1");
    }
    if (multi_uv)
    {
        variant.addPermutation("MULTI_UV", "1");
    }

    if (!alpha_blend)
    {
        return variant.createShader();
    }

    variant.addPermutation("ALPHA_BLEND", "1");
    variant.mFeatures.calculatesLighting = false;
    variant.mFeatures.hasLighting = false;
    variant.mFeatures.isAlphaLighting = true;
    variant.mFeatures.hasSrgb = true;
    variant.mFeatures.calculatesAtmospherics = true;
    variant.mFeatures.hasAtmospherics = true;
    variant.mFeatures.hasGamma = true;
    variant.mFeatures.hasShadows = use_sun_shadow;
    variant.mFeatures.isDeferred = true;
    variant.mFeatures.hasReflectionProbes = true;

    if (use_sun_shadow)
    {
        variant.addPermutation("HAS_SUN_SHADOW", "1");
    }

    const bool success = variant.createShader();
    llassert(success);

    // Alpha Shader Hack; see LLRender::syncMatrices().
    variant.mFeatures.calculatesLighting = true;
    variant.mFeatures.hasLighting = true;
    return success;
}

// Builds the complete GLTF permutation table and reports whether every variant succeeded.
bool makeGLTFVariants(LLGLSLShader& shader, bool use_sun_shadow)
{
    shader.mFeatures.mGLTF = true;
    shader.mGLTFVariants.resize(LLGLSLShader::NUM_GLTF_VARIANTS);

    for (U32 i = 0; i < LLGLSLShader::NUM_GLTF_VARIANTS; ++i)
    {
        const bool alpha_blend = i & LLGLSLShader::GLTFVariant::ALPHA_BLEND;
        const bool rigged = i & LLGLSLShader::GLTFVariant::RIGGED;
        const bool unlit = i & LLGLSLShader::GLTFVariant::UNLIT;
        const bool multi_uv = i & LLGLSLShader::GLTFVariant::MULTI_UV;

        if (!makeGLTFVariant(shader, shader.mGLTFVariants[i], alpha_blend, rigged,
                             unlit, multi_uv, use_sun_shadow))
        {
            return false;
        }
    }
    return true;
}
}

// Exact OIT-owned shader objects.
LLGLSLShader gExactOITGLTFProgram;
LLGLSLShader gExactOITEmissiveProgram;
LLGLSLShader gExactOITSkinnedEmissiveProgram;
LLGLSLShader gExactOITPBRGlowProgram;
LLGLSLShader gExactOITSkinnedPBRGlowProgram;
LLGLSLShader gExactOITCompositeProgram;
LLGLSLShader gExactOITClassifyProgram;
LLGLSLShader gExactOITBlockSortProgram;
LLGLSLShader gExactOITMergeProgram;
LLGLSLShader gExactOITAlphaProgram;
LLGLSLShader gExactOITSkinnedAlphaProgram;
LLGLSLShader gExactOITPBRAlphaProgram;
LLGLSLShader gExactOITSkinnedPBRAlphaProgram;
LLGLSLShader gExactOITFullbrightAlphaProgram;
LLGLSLShader gExactOITSkinnedFullbrightAlphaProgram;
LLGLSLShader gExactOITMaterialAlphaProgram[LLMaterial::SHADER_COUNT * 2];

bool FSExactOIT::sCaptureCompleted = false;
bool FSExactOIT::sCaptureClearNeeded = false;
bool FSExactOIT::sVanillaFallbackActive = false;
bool FSExactOIT::sCaptureActive = false;
bool FSExactOIT::sRuntimeAllocationAttempted = false;
bool FSExactOIT::sRetainNodePoolOnRelease = false;
LLRenderTarget FSExactOIT::sOpaqueTarget;
FSExactOIT::Resources FSExactOIT::sResources;

// Returns the explicit cache salt for the current Exact OIT shader composition.
const char* FSExactOIT::shaderCacheRevision()
{
    // Shader paths alone do not invalidate cached program binaries after
    // source or layout changes in same-version development builds.
    // Keep development builds from reusing incompatible Exact OIT shader binaries.
    return "Exact OIT shader revision v17";
}

// Reports whether the active OpenGL and GLSL versions provide required Exact OIT features.
bool FSExactOIT::isSupported()
{
    return gGLManager.mGLVersion >= 4.29f &&
        (gGLManager.mGLSLVersionMajor > 4 ||
         (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 30));
}

// Reports whether Exact OIT is both requested by the user and supported by the GPU.
bool FSExactOIT::isEnabled()
{
    return gSavedSettings.getBOOL("RenderExactOIT") && isSupported();
}

// Loads the complete Exact OIT shader family and propagates the aggregate success state.
bool FSExactOIT::loadShaders(bool success, S32 shader_level, bool use_sun_shadow,
                             bool gltf_enabled, std::vector<LLGLSLShader*>& shader_list)
{
    if (!isSupported() || !success)
    {
        return success;
    }

    if (gltf_enabled) success = loadGLTFShaders(shader_level, use_sun_shadow);
    if (success) success = loadPBRGlowShaders(shader_level);
    if (success) success = loadAlphaShaders(shader_level, use_sun_shadow);
    if (success) success = loadPBRAlphaShaders(shader_level, use_sun_shadow);
    if (success) success = loadFullbrightAlphaShaders(shader_level);
    if (success) success = loadMaterialAlphaShaders(shader_level, use_sun_shadow, shader_list);
    if (success) success = loadEmissiveShaders(shader_level);
    if (success) success = loadCompositeShader(shader_level);
    if (success) loadComputeSortShaders(shader_level);
    return success;
}

// Adds all persistent Exact OIT program objects to the viewer shader registry.
void FSExactOIT::registerShaders(std::vector<LLGLSLShader*>& shader_list)
{
    shader_list.push_back(&gExactOITCompositeProgram);
    shader_list.push_back(&gExactOITClassifyProgram);
    shader_list.push_back(&gExactOITBlockSortProgram);
    shader_list.push_back(&gExactOITMergeProgram);
    shader_list.push_back(&gExactOITAlphaProgram);
    shader_list.push_back(&gExactOITSkinnedAlphaProgram);
    shader_list.push_back(&gExactOITPBRAlphaProgram);
    shader_list.push_back(&gExactOITSkinnedPBRAlphaProgram);
    shader_list.push_back(&gExactOITFullbrightAlphaProgram);
    shader_list.push_back(&gExactOITSkinnedFullbrightAlphaProgram);
    shader_list.push_back(&gExactOITGLTFProgram);
    shader_list.push_back(&gExactOITEmissiveProgram);
    shader_list.push_back(&gExactOITSkinnedEmissiveProgram);
    shader_list.push_back(&gExactOITPBRGlowProgram);
    shader_list.push_back(&gExactOITSkinnedPBRGlowProgram);
}

// Unloads every Exact OIT shader and rigged or material variant.
void FSExactOIT::unloadShaders()
{
    gExactOITCompositeProgram.unload();
    gExactOITClassifyProgram.unload();
    gExactOITBlockSortProgram.unload();
    gExactOITMergeProgram.unload();
    gExactOITAlphaProgram.unload();
    gExactOITSkinnedAlphaProgram.unload();
    gExactOITPBRAlphaProgram.unload();
    gExactOITSkinnedPBRAlphaProgram.unload();
    gExactOITFullbrightAlphaProgram.unload();
    gExactOITSkinnedFullbrightAlphaProgram.unload();
    for (LLGLSLShader& shader : gExactOITMaterialAlphaProgram)
    {
        shader.unload();
    }
    gExactOITGLTFProgram.unload();
    gExactOITEmissiveProgram.unload();
    gExactOITSkinnedEmissiveProgram.unload();
    gExactOITPBRGlowProgram.unload();
    gExactOITSkinnedPBRGlowProgram.unload();
    sBlendCache.clear();
}

// Creates the Exact OIT GLTF base program and all required feature variants.
bool FSExactOIT::loadGLTFShaders(S32 shader_level, bool use_sun_shadow)
{
    gExactOITGLTFProgram.mName = "Exact OIT GLTF PBR Metallic Roughness Shader";
    gExactOITGLTFProgram.mFeatures.hasSrgb = true;
    gExactOITGLTFProgram.mShaderFiles.clear();
    gExactOITGLTFProgram.mShaderFiles.emplace_back("gltf/pbrmetallicroughnessV.glsl", GL_VERTEX_SHADER);
    gExactOITGLTFProgram.mShaderFiles.emplace_back("gltf/pbrmetallicroughnessF.glsl", GL_FRAGMENT_SHADER);
    addCaptureFragment(gExactOITGLTFProgram);
    gExactOITGLTFProgram.mShaderLevel = shader_level;
    gExactOITGLTFProgram.clearPermutations();
    addExactOITPermutations(gExactOITGLTFProgram);
    addCommonPermutations(gExactOITGLTFProgram);

    const bool success = makeGLTFVariants(gExactOITGLTFProgram, use_sun_shadow);
    llassert(success);
    return success;
}

// Creates the PBR glow capture shader and its rigged variant.
bool FSExactOIT::loadPBRGlowShaders(S32 shader_level)
{
    gExactOITPBRGlowProgram.mName = "Exact OIT PBR Glow Shader";
    gExactOITPBRGlowProgram.mFeatures.hasSrgb = true;
    gExactOITPBRGlowProgram.mShaderFiles.clear();
    gExactOITPBRGlowProgram.mShaderFiles.emplace_back("deferred/pbrglowV.glsl", GL_VERTEX_SHADER);
    gExactOITPBRGlowProgram.mShaderFiles.emplace_back("deferred/exactOITPbrGlowF.glsl", GL_FRAGMENT_SHADER);
    gExactOITPBRGlowProgram.mShaderLevel = shader_level;
    addCommonPermutations(gExactOITPBRGlowProgram);

    bool success = makeRiggedVariant(gExactOITPBRGlowProgram, gExactOITSkinnedPBRGlowProgram);
    success = success && gExactOITPBRGlowProgram.createShader();
    llassert(success);
    return success;
}

// Creates the legacy emissive capture shader and its rigged variant.
bool FSExactOIT::loadEmissiveShaders(S32 shader_level)
{
    gExactOITEmissiveProgram.mName = "Exact OIT Emissive Shader";
    gExactOITEmissiveProgram.mFeatures.calculatesAtmospherics = true;
    gExactOITEmissiveProgram.mFeatures.hasGamma = true;
    gExactOITEmissiveProgram.mFeatures.hasAtmospherics = true;
    gExactOITEmissiveProgram.mFeatures.mIndexedTextureChannels = LLGLSLShader::sIndexedTextureChannels;
    gExactOITEmissiveProgram.mShaderFiles.clear();
    gExactOITEmissiveProgram.mShaderFiles.emplace_back("deferred/emissiveV.glsl", GL_VERTEX_SHADER);
    gExactOITEmissiveProgram.mShaderFiles.emplace_back("deferred/exactOITEmissiveF.glsl", GL_FRAGMENT_SHADER);
    gExactOITEmissiveProgram.mShaderLevel = shader_level;
    addCommonPermutations(gExactOITEmissiveProgram);

    bool success = makeRiggedVariant(gExactOITEmissiveProgram, gExactOITSkinnedEmissiveProgram);
    success = success && gExactOITEmissiveProgram.createShader();
    llassert(success);
    return success;
}

// Creates the fullscreen sorting and final-composite shader.
bool FSExactOIT::loadCompositeShader(S32 shader_level)
{
    gExactOITCompositeProgram.mName = "Exact OIT Composite Shader";
    gExactOITCompositeProgram.mFeatures.isDeferred = true;
    gExactOITCompositeProgram.mShaderFiles.clear();
    gExactOITCompositeProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    gExactOITCompositeProgram.mShaderFiles.emplace_back("deferred/exactOITCompositeF.glsl", GL_FRAGMENT_SHADER);
    gExactOITCompositeProgram.mShaderLevel = shader_level;

    const bool success = gExactOITCompositeProgram.createShader();
    llassert(success);
    return success;
}

// Creates optional compute stages; failure leaves the proven fullscreen sorter available.
void FSExactOIT::loadComputeSortShaders(S32 shader_level)
{
    struct ComputeStage
    {
        LLGLSLShader* shader;
        const char* name;
        const char* permutation;
    };
    const ComputeStage stages[] = {
        { &gExactOITClassifyProgram, "Exact OIT Compute Classifier", "OIT_CLASSIFY" },
        { &gExactOITBlockSortProgram, "Exact OIT Compute Block Sort", "OIT_BLOCK_SORT" },
        { &gExactOITMergeProgram, "Exact OIT Compute Merge", "OIT_MERGE" }
    };

    for (const ComputeStage& stage : stages)
    {
        stage.shader->mName = stage.name;
        stage.shader->mFeatures.attachNothing = true;
        stage.shader->mShaderFiles.clear();
        stage.shader->mShaderFiles.emplace_back("deferred/exactOITSortC.glsl", GL_COMPUTE_SHADER);
        stage.shader->mShaderLevel = shader_level;
        stage.shader->clearPermutations();
        stage.shader->addPermutation(stage.permutation, "1");
        if (!stage.shader->createShader())
        {
            gExactOITClassifyProgram.unload();
            gExactOITBlockSortProgram.unload();
            gExactOITMergeProgram.unload();
            LL_WARNS("ExactOIT") << "Optional compute sorter unavailable; using fullscreen sorter"
                                 << LL_ENDL;
            return;
        }
    }
}

// Creates ordinary and skinned deferred-alpha capture programs.
bool FSExactOIT::loadAlphaShaders(S32 shader_level, bool use_sun_shadow)
{
    LLGLSLShader* shaders[] = { &gExactOITAlphaProgram, &gExactOITSkinnedAlphaProgram };
    bool success = true;

    for (U32 i = 0; i < 2 && success; ++i)
    {
        const bool rigged = i == 1;
        LLGLSLShader& shader = *shaders[i];
        shader.mName = rigged ? "Skinned Deferred Alpha Exact OIT Shader" : "Deferred Alpha Exact OIT Shader";
        shader.mFeatures.calculatesLighting = false;
        shader.mFeatures.hasLighting = false;
        shader.mFeatures.isAlphaLighting = true;
        shader.mFeatures.hasSrgb = true;
        shader.mFeatures.calculatesAtmospherics = true;
        shader.mFeatures.hasAtmospherics = true;
        shader.mFeatures.hasGamma = true;
        shader.mFeatures.hasShadows = use_sun_shadow;
        shader.mFeatures.hasReflectionProbes = true;
        shader.mFeatures.mIndexedTextureChannels = LLGLSLShader::sIndexedTextureChannels;
        if (rigged)
        {
            shader.mFeatures.hasObjectSkinning = true;
        }
        shader.mShaderFiles.clear();
        shader.mShaderFiles.emplace_back("deferred/alphaV.glsl", GL_VERTEX_SHADER);
        shader.mShaderFiles.emplace_back("deferred/alphaF.glsl", GL_FRAGMENT_SHADER);
        addCaptureFragment(shader);
        shader.clearPermutations();
        shader.addPermutation("USE_VERTEX_COLOR", "1");
        shader.addPermutation("HAS_ALPHA_MASK", "1");
        shader.addPermutation("USE_INDEXED_TEX", "1");
        addExactOITPermutations(shader);
        if (use_sun_shadow)
        {
            shader.addPermutation("HAS_SUN_SHADOW", "1");
        }
        if (rigged)
        {
            shader.addPermutation("HAS_SKIN", "1");
        }
        addCommonPermutations(shader);
        shader.mShaderLevel = shader_level;
        success = shader.createShader();
        llassert(success);
        shader.mFeatures.calculatesLighting = true;
        shader.mFeatures.hasLighting = true;
    }

    gExactOITAlphaProgram.mRiggedVariant = &gExactOITSkinnedAlphaProgram;
    return success;
}

// Creates ordinary and skinned PBR-alpha capture programs.
bool FSExactOIT::loadPBRAlphaShaders(S32 shader_level, bool use_sun_shadow)
{
    LLGLSLShader* shaders[] = { &gExactOITPBRAlphaProgram, &gExactOITSkinnedPBRAlphaProgram };
    bool success = true;

    for (U32 i = 0; i < 2 && success; ++i)
    {
        const bool rigged = i == 1;
        LLGLSLShader& shader = *shaders[i];
        shader.mName = rigged ? "Skinned Deferred PBR Alpha Exact OIT Shader" : "Deferred PBR Alpha Exact OIT Shader";
        shader.mFeatures.calculatesLighting = false;
        shader.mFeatures.hasLighting = false;
        shader.mFeatures.isAlphaLighting = true;
        shader.mFeatures.hasSrgb = true;
        shader.mFeatures.calculatesAtmospherics = true;
        shader.mFeatures.hasAtmospherics = true;
        shader.mFeatures.hasGamma = true;
        shader.mFeatures.hasShadows = use_sun_shadow;
        shader.mFeatures.isDeferred = true;
        shader.mFeatures.hasReflectionProbes = shader_level;
        if (rigged)
        {
            shader.mFeatures.hasObjectSkinning = true;
        }
        shader.mShaderFiles.clear();
        shader.mShaderFiles.emplace_back("deferred/pbralphaV.glsl", GL_VERTEX_SHADER);
        shader.mShaderFiles.emplace_back("deferred/pbralphaF.glsl", GL_FRAGMENT_SHADER);
        addCaptureFragment(shader);
        shader.clearPermutations();
        shader.addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", (int)LLMaterial::DIFFUSE_ALPHA_MODE_BLEND));
        shader.addPermutation("HAS_NORMAL_MAP", "1");
        shader.addPermutation("HAS_SPECULAR_MAP", "1");
        shader.addPermutation("HAS_EMISSIVE_MAP", "1");
        shader.addPermutation("USE_VERTEX_COLOR", "1");
        addExactOITPermutations(shader);
        if (use_sun_shadow)
        {
            shader.addPermutation("HAS_SUN_SHADOW", "1");
        }
        if (rigged)
        {
            shader.addPermutation("HAS_SKIN", "1");
        }
        addCommonPermutations(shader);
        shader.mShaderLevel = shader_level;
        success = shader.createShader();
        llassert(success);
        shader.mFeatures.calculatesLighting = true;
        shader.mFeatures.hasLighting = true;
    }

    gExactOITPBRAlphaProgram.mRiggedVariant = &gExactOITSkinnedPBRAlphaProgram;
    return success;
}

// Creates ordinary and skinned fullbright-alpha capture programs.
bool FSExactOIT::loadFullbrightAlphaShaders(S32 shader_level)
{
    LLGLSLShader* shaders[] = { &gExactOITFullbrightAlphaProgram, &gExactOITSkinnedFullbrightAlphaProgram };
    bool success = true;

    for (U32 i = 0; i < 2 && success; ++i)
    {
        const bool rigged = i == 1;
        LLGLSLShader& shader = *shaders[i];
        shader.mName = rigged ? "Skinned Deferred Fullbright Alpha Exact OIT Shader" : "Deferred Fullbright Alpha Exact OIT Shader";
        shader.mFeatures.calculatesAtmospherics = true;
        shader.mFeatures.hasGamma = true;
        shader.mFeatures.hasAtmospherics = true;
        shader.mFeatures.hasSrgb = true;
        shader.mFeatures.isDeferred = true;
        shader.mFeatures.mIndexedTextureChannels = LLGLSLShader::sIndexedTextureChannels;
        if (rigged)
        {
            shader.mFeatures.hasObjectSkinning = true;
        }
        shader.mShaderFiles.clear();
        shader.mShaderFiles.emplace_back("deferred/fullbrightV.glsl", GL_VERTEX_SHADER);
        shader.mShaderFiles.emplace_back("deferred/fullbrightF.glsl", GL_FRAGMENT_SHADER);
        addCaptureFragment(shader);
        shader.clearPermutations();
        shader.addPermutation("HAS_ALPHA_MASK", "1");
        shader.addPermutation("IS_ALPHA", "1");
        addExactOITPermutations(shader);
        if (rigged)
        {
            shader.addPermutation("HAS_SKIN", "1");
        }
        addCommonPermutations(shader);
        shader.mShaderLevel = shader_level;
        success = shader.createShader();
        llassert(success);
    }

    gExactOITFullbrightAlphaProgram.mRiggedVariant = &gExactOITSkinnedFullbrightAlphaProgram;
    return success;
}

// Creates every supported material capture permutation and returns aggregate success.
bool FSExactOIT::loadMaterialAlphaShaders(S32 shader_level, bool use_sun_shadow,
                                          std::vector<LLGLSLShader*>& shader_list)
{
    bool success = true;

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2 && success; ++i)
    {
        const U32 alpha_mode = i & 0x3;
        if (alpha_mode != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            continue;
        }

        const bool has_skin = i >= LLMaterial::SHADER_COUNT;
        const U32 idx = i & 0xf;
        LLGLSLShader& shader = gExactOITMaterialAlphaProgram[i];

        if (!has_skin)
        {
            shader_list.push_back(&shader);
        }
        shader.mName = llformat("Material Exact OIT Shader %d", i);
        shader.mFeatures.hasSrgb = true;
        shader.mFeatures.calculatesAtmospherics = true;
        shader.mFeatures.hasAtmospherics = true;
        shader.mFeatures.hasGamma = true;
        shader.mFeatures.hasShadows = use_sun_shadow;
        shader.mFeatures.hasReflectionProbes = true;
        if (has_skin)
        {
            shader.mFeatures.hasObjectSkinning = true;
        }
        shader.mShaderFiles.clear();
        shader.mShaderFiles.emplace_back("deferred/materialV.glsl", GL_VERTEX_SHADER);
        shader.mShaderFiles.emplace_back("deferred/materialF.glsl", GL_FRAGMENT_SHADER);
        addCaptureFragment(shader);
        shader.mShaderLevel = shader_level;
        shader.clearPermutations();
        if (idx & 0x8)
        {
            shader.addPermutation("HAS_NORMAL_MAP", "1");
        }
        if (idx & 0x4)
        {
            shader.addPermutation("HAS_SPECULAR_MAP", "1");
        }
        shader.addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", alpha_mode));
        shader.addPermutation("HAS_ALPHA_MASK", "1");
        addExactOITPermutations(shader);
        if (use_sun_shadow)
        {
            shader.addPermutation("HAS_SUN_SHADOW", "1");
        }
        addCommonPermutations(shader);
        if (has_skin)
        {
            shader.addPermutation("HAS_SKIN", "1");
        }
        else
        {
            shader.mRiggedVariant = &gExactOITMaterialAlphaProgram[i + LLMaterial::SHADER_COUNT];
        }
        success = shader.createShader();
        llassert(success);
        shader.mFeatures.hasLighting = true;
    }

    return success;
}

// Appends current availability, capacity, demand, overflow, and status data to the viewer report.
void FSExactOIT::appendDiagnostics(LLSD& info)
{
    info["EXACT_OIT_AVAILABLE"] = sResources.available;
    info["EXACT_OIT_NODE_CAPACITY"] = LLSD::Integer(sResources.capacity);
    info["EXACT_OIT_PEAK_NODES"] = LLSD::Integer(sResources.peakNodes);
    info["EXACT_OIT_OVERFLOW_COUNT"] = LLSD::Integer(sResources.overflowCount);
    info["EXACT_OIT_MEMORY_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.capacity) * 32ull) / (1024ull * 1024ull));
    info["EXACT_OIT_COMPUTE_SORT_AVAILABLE"] = sResources.computeSortAvailable;
    info["EXACT_OIT_COMPUTE_QUEUE_MB"] = LLSD::Integer(
        (static_cast<U64>(sResources.sortQueueCapacity) * 8ull) / (1024ull * 1024ull));

    if (gGLManager.mGLVersion < 4.29f)
    {
        info["EXACT_OIT_STATUS"] = "Unavailable: OpenGL 4.3 is required";
    }
    else if (!sResources.available)
    {
        info["EXACT_OIT_STATUS"] = "Unavailable: GPU resource allocation failed or safe VRAM limit reached";
    }
    else if (!gExactOITCompositeProgram.mProgramObject)
    {
        info["EXACT_OIT_STATUS"] = "Unavailable: exact OIT shader creation failed";
    }
    else
    {
        info["EXACT_OIT_STATUS"] = "Available";
    }
}

// Resets transient capture and fallback state at the start of a transparency frame.
void FSExactOIT::beginFrame()
{
    // Mode-transition invalidation is centralized in the neutral dispatcher.
    sCaptureActive = false;
    sCaptureCompleted = false;
    sCaptureClearNeeded = true;
    sVanillaFallbackActive = false;

    // Deferred node-pool growth (see pendingGrowthNodes): the previous
    // frame's GPU work (capture, any speculative/natural sort passes,
    // composite) was already issued in submission order before this call, so
    // reallocating sResources.nodes here cannot race it. This frame's own
    // capture draws have not been issued yet. Reallocating mid-frame instead
    // (right after that frame's own capture) raced GPU work still queued
    // against the buffer -- see doc/ayanestorm-oit-e1-e4-growth-race.md.
    // Guarded by isEnabled() && nodes: a pending request must not trigger a
    // multi-GB glBufferData on the frame Exact OIT gets disabled or its
    // resources are already being torn down.
    if (sResources.pendingGrowthNodes > 0 && isEnabled() && sResources.nodes)
    {
        if (!growNodePool(sResources.pendingGrowthNodes))
        {
            // Grew (or tried to), but the safe VRAM cap still leaves
            // capacity short of what was requested; captureOverflowed() /
            // the predictive-skip policy handle the ongoing consequence,
            // this just makes the ceiling visible in the log.
            LL_INFOS("ExactOIT") << "Exact OIT node pool grew to " << sResources.capacity
                << " but requested " << sResources.pendingGrowthNodes
                << " remains short of the safe VRAM cap." << LL_ENDL;
        }
    }
    sResources.pendingGrowthNodes = 0;

    // Decremented once per frame here (captureEligible() runs for both alpha
    // pools and must not double-decrement). The debug overlay wants to see
    // every frame's diagnostics, so the predictive skip never engages while
    // it is on; only log.
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderExactOITDebugMode", 0);
    if (sResources.skipFramesRemaining > 0)
    {
        if (debug_mode != 0)
        {
            sResources.skipFramesRemaining = 0;
        }
        else
        {
            --sResources.skipFramesRemaining;
        }
    }
}

// Reports whether this frame produced a complete Exact OIT capture.
bool FSExactOIT::captureCompleted()
{
    return sCaptureCompleted;
}

// Reports whether draw submission is currently writing Exact OIT nodes.
bool FSExactOIT::captureActive()
{
    return sCaptureActive;
}

// Marks the current frame as having completed its Exact OIT capture traversal.
void FSExactOIT::markCaptureCompleted()
{
    sCaptureCompleted = true;
}

// Invalidates the current capture so it cannot be composited.
void FSExactOIT::discardCapture()
{
    sCaptureCompleted = false;
}

// Enables or disables the guarded same-frame vanilla fallback state.
void FSExactOIT::setVanillaFallback(bool active)
{
    sVanillaFallbackActive = active;
}

// Enters vanilla fallback mode for the lifetime of this scope.
FSExactOIT::VanillaFallbackScope::VanillaFallbackScope()
{
    FSExactOIT::setVanillaFallback(true);
}

// Leaves vanilla fallback mode when fallback traversal finishes.
FSExactOIT::VanillaFallbackScope::~VanillaFallbackScope()
{
    FSExactOIT::setVanillaFallback(false);
}

// Marks subsequent alpha and GLTF draws as Exact OIT capture draws.
void FSExactOIT::beginCapture()
{
    sCaptureActive = true;
}

// Restores ordinary draw routing after Exact OIT capture traversal.
void FSExactOIT::endCapture()
{
    sCaptureActive = false;
}

// Enters Exact OIT capture mode for this traversal scope.
FSExactOIT::CaptureScope::CaptureScope()
{
    beginCapture();
}

// Leaves Exact OIT capture mode when traversal exits, including early unwinding.
FSExactOIT::CaptureScope::~CaptureScope()
{
    endCapture();
}

// Clears per-frame images and counters once, then binds capture images and buffers.
void FSExactOIT::prepareCaptureBuffers()
{
    if (!sCaptureClearNeeded)
    {
        return;
    }

    const GLint previous_fbo = LLRenderTarget::sCurFBO;
    const GLuint empty[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
    const GLuint zero[4] = { 0u, 0u, 0u, 0u };
    glBindFramebuffer(GL_FRAMEBUFFER, sResources.headFBO);
    glClearBufferuiv(GL_COLOR, 0, empty);
    glClearBufferuiv(GL_COLOR, 1, zero);
    glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);

    const U32 control[4] = { 0, sResources.capacity, 0, 0 };
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(control), control);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindImageTexture(0, sResources.heads, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(1, sResources.counts, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sResources.nodes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sResources.control);
    sCaptureClearNeeded = false;
}

// Validates the complete shader family and returns false when any required program is missing.
bool FSExactOIT::shadersReady()
{
    std::string missing;
    auto require = [&missing](const LLGLSLShader& shader, const char* name)
    {
        if (missing.empty() && !shader.mProgramObject)
        {
            missing = name;
        }
    };

    require(gExactOITAlphaProgram, "deferred alpha");
    require(gExactOITSkinnedAlphaProgram, "skinned deferred alpha");
    require(gExactOITPBRAlphaProgram, "PBR alpha");
    require(gExactOITSkinnedPBRAlphaProgram, "skinned PBR alpha");
    require(gExactOITFullbrightAlphaProgram, "fullbright alpha");
    require(gExactOITSkinnedFullbrightAlphaProgram, "skinned fullbright alpha");
    require(gExactOITEmissiveProgram, "emissive");
    if (missing.empty() && (!gExactOITEmissiveProgram.mRiggedVariant ||
        !gExactOITEmissiveProgram.mRiggedVariant->mProgramObject)) missing = "skinned emissive";
    require(gExactOITPBRGlowProgram, "PBR glow");
    if (missing.empty() && (!gExactOITPBRGlowProgram.mRiggedVariant ||
        !gExactOITPBRGlowProgram.mRiggedVariant->mProgramObject)) missing = "skinned PBR glow";
    require(gExactOITCompositeProgram, "composite");

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT * 2 && missing.empty(); ++i)
    {
        if ((i & 0x3) == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND &&
            !gExactOITMaterialAlphaProgram[i].mProgramObject)
        {
            missing = llformat("material alpha %u", i);
        }
    }
    static LLCachedControl<bool> gltf_enabled(gSavedSettings, "GLTFEnabled", false);
    if (missing.empty() && gltf_enabled)
    {
        if (gExactOITGLTFProgram.mGLTFVariants.empty()) missing = "GLTF variants";
        for (const LLGLSLShader& shader : gExactOITGLTFProgram.mGLTFVariants)
        {
            if (!shader.mProgramObject)
            {
                missing = "GLTF variant";
                break;
            }
        }
    }

    if (!missing.empty() && isEnabled())
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LL_WARNS("ExactOIT") << "Exact OIT shader set is incomplete (missing " << missing
                                  << "); using complete vanilla transparency." << LL_ENDL;
        }
    }
    return missing.empty();
}

// Decides whether capture may run, handling runtime allocation and disable transitions.
bool FSExactOIT::captureEligible(bool rendering_huds, bool impostor_render, bool cube_snapshot,
                                 U32 width, U32 height)
{
    if (!isEnabled())
    {
        if (sResources.available || sResources.nodes || sOpaqueTarget.isComplete())
        {
            const GLint previous_fbo = LLRenderTarget::sCurFBO;
            releaseResources(false);
            glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
        }
        sRuntimeAllocationAttempted = false;
        return false;
    }

    if (!sResources.available && !sRuntimeAllocationAttempted &&
        !rendering_huds && !impostor_render && !cube_snapshot)
    {
        sRuntimeAllocationAttempted = true;
        const GLint previous_fbo = LLRenderTarget::sCurFBO;
        allocateResources(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
    }

    if (sResources.skipFramesRemaining > 0)
    {
        return false;
    }

    return shadersReady() && sResources.available &&
        !rendering_huds && !impostor_render && !cube_snapshot && !sVanillaFallbackActive;
}

// Uploads the normal alpha-pass environment state to every Exact OIT capture shader.
void FSExactOIT::prepareCaptureShaders(PrepareShader prepare, F32 water_sign)
{
    prepare(&gExactOITAlphaProgram, true, water_sign);
    prepare(&gExactOITPBRAlphaProgram, true, water_sign);
    prepare(&gExactOITFullbrightAlphaProgram, true, water_sign);
    for (LLGLSLShader& shader : gExactOITMaterialAlphaProgram)
    {
        if (shader.mProgramObject)
        {
            prepare(&shader, true, water_sign);
        }
    }
    prepare(&gExactOITEmissiveProgram, false, water_sign);
    prepare(&gExactOITPBRGlowProgram, false, water_sign);
}

// Executes eligible post-water capture traversal and returns whether it replaced vanilla traversal.
bool FSExactOIT::renderPostDeferredCapture(LLDrawPoolAlpha& pool, PrepareShader prepare,
                                           F32 water_sign, LLGLSLShader*& emissive_shader,
                                           LLGLSLShader*& pbr_emissive_shader)
{
    const bool capture_ready = captureEligible(
        LLPipeline::sRenderingHUDs, LLPipeline::sImpostorRender, gCubeSnapshot,
        gPipeline.mRT->screen.getWidth(), gPipeline.mRT->screen.getHeight());
    if (!capture_ready || pool.getType() != LLDrawPool::POOL_ALPHA_POST_WATER)
    {
        return false;
    }

    prepareCaptureShaders(prepare, water_sign);
    emissive_shader = emissiveShader();
    pbr_emissive_shader = pbrGlowShader();
    LLGLSLShader::unbind();

    LL_PROFILE_GPU_ZONE("Exact OIT capture");
    prepareCaptureBuffers();
    {
        CaptureScope capture_scope;
        pool.forwardRender(true);
        pool.forwardRender(false);
    }
    markCaptureCompleted();

    if (sResources.readbackMapped)
    {
        // Make the fragments' SSBO atomics visible to the copy below, then
        // copy the four control words into the host-visible readback buffer
        // on the GPU. The control buffer itself is never mapped (see
        // allocateNodePool()); this copy is the only host-visible path.
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_COPY_READ_BUFFER, sResources.control);
        glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 4 * sizeof(U32));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    // Fence marks "capture (and, if any, the readback copy) is done" on the
    // GPU timeline. The CPU only waits on this fence (in waitValidation(),
    // after sort pass 1 has already been issued), not on the full pipeline,
    // so the GPU can keep working while validation is pending.
    if (sResources.captureFence)
    {
        glDeleteSync(sResources.captureFence);
    }
    sResources.captureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    return true;
}

// Uploads per-draw blend and glow data when capturing; returns true when vanilla blending is suppressed.
bool FSExactOIT::configureCapturedDrawIfActive(LLGLSLShader* shader, U32 color_source,
                                               U32 color_destination, U32 alpha_source,
                                               U32 alpha_destination)
{
    if (!sCaptureActive)
    {
        return false;
    }
    if (!shader)
    {
        return true;
    }

    const U32 packed_blend = color_source | (color_destination << 8) |
        (alpha_source << 16) | (alpha_destination << 24);
    uploadBlendFactors(*shader, packed_blend);
    return true;
}

// Dispatches captured emissive lists and returns true when the vanilla emissive block must be skipped.
bool FSExactOIT::handleCapturedEmissives(LLDrawPoolAlpha& pool, bool depth_only,
                                         std::vector<LLDrawInfo*>& emissives,
                                         std::vector<LLDrawInfo*>& pbr_emissives,
                                         std::vector<LLDrawInfo*>& rigged_emissives,
                                         std::vector<LLDrawInfo*>& pbr_rigged_emissives)
{
    if (depth_only)
    {
        return true;
    }
    if (!sCaptureActive)
    {
        return false;
    }

    if (!emissives.empty()) pool.renderEmissives(emissives);
    if (!pbr_emissives.empty()) pool.renderPbrEmissives(pbr_emissives);
    if (!rigged_emissives.empty()) pool.renderRiggedEmissives(rigged_emissives);
    if (!pbr_rigged_emissives.empty()) pool.renderRiggedPbrEmissives(pbr_rigged_emissives);
    return true;
}

// Uploads the standard alpha blend tuple and zero glow for a captured GLTF draw.
void FSExactOIT::configureGLTFCapturedDraw(LLGLSLShader& shader)
{
    const U32 packed_blend = U32(LLRender::BF_SOURCE_ALPHA) |
        (U32(LLRender::BF_ONE_MINUS_SOURCE_ALPHA) << 8) |
        (U32(LLRender::BF_ZERO) << 16) |
        (U32(LLRender::BF_ONE_MINUS_SOURCE_ALPHA) << 24);
    uploadBlendFactors(shader, packed_blend);
}

// Returns the Exact OIT GLTF program during capture, otherwise the supplied vanilla program.
LLGLSLShader& FSExactOIT::gltfProgram(LLGLSLShader& ordinary_program)
{
    return sCaptureActive ? gExactOITGLTFProgram : ordinary_program;
}

// Returns the capture alpha shader while active, otherwise the supplied ordinary shader.
LLGLSLShader* FSExactOIT::alphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gExactOITAlphaProgram : ordinary;
}

// Returns the capture PBR-alpha shader while active, otherwise the supplied ordinary shader.
LLGLSLShader* FSExactOIT::pbrAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gExactOITPBRAlphaProgram : ordinary;
}

// Returns the capture fullbright shader while active, otherwise the supplied ordinary shader.
LLGLSLShader* FSExactOIT::fullbrightAlphaShader(LLGLSLShader* ordinary)
{
    return sCaptureActive ? &gExactOITFullbrightAlphaProgram : ordinary;
}

// Returns the requested capture material variant when valid, otherwise the ordinary shader.
LLGLSLShader* FSExactOIT::materialAlphaShader(U32 mask, LLGLSLShader* ordinary)
{
    LLGLSLShader& shader = gExactOITMaterialAlphaProgram[mask];
    return sCaptureActive && shader.mProgramObject ? &shader : ordinary;
}

// Returns the Exact OIT emissive capture shader selected for alpha traversal.
LLGLSLShader* FSExactOIT::emissiveShader()
{
    return &gExactOITEmissiveProgram;
}

// Returns the Exact OIT PBR glow capture shader selected for alpha traversal.
LLGLSLShader* FSExactOIT::pbrGlowShader()
{
    return &gExactOITPBRGlowProgram;
}

// Returns the node capacity this session may safely allocate (VRAM-bounded).
static U64 safeNodeCapacity()
{
    constexpr U64 node_bytes = 32;
    const U64 vram_bytes = static_cast<U64>(gGLManager.mVRAM) * 1024u * 1024u;
    return llmin<U64>(vram_bytes / 4u, 2ull * 1024ull * 1024ull * 1024ull) / node_bytes;
}

// Computes the capacity growNodePool() would grow to for `required_nodes`,
// without performing any GL call. Pure policy, safe to call at any time.
static U32 computeGrownCapacity(U32 required_nodes, U32 current_capacity)
{
    const U64 safe_nodes = safeNodeCapacity();
    const U64 demand_with_headroom = (static_cast<U64>(required_nodes) * 5ull + 3ull) / 4ull;
    const U64 geometric_growth = static_cast<U64>(current_capacity) * 2ull;
    const U64 requested = llmax(demand_with_headroom, geometric_growth);
    return U32(llmin<U64>(requested, llmin<U64>(safe_nodes, 0xfffffffeull)));
}

// Grows the node pool toward `required_nodes` (with headroom) when capacity
// allows it (a glBufferData orphan). Caller must guarantee no GPU work from
// this or an earlier frame is still queued against sResources.nodes (see
// pendingGrowthNodes / beginFrame()): reallocating while capture, a
// speculative sort pass, or an abandoned overflow frame's sort work is still
// in flight races that GPU work against the new (garbage) storage.
// Returns true if capacity now covers `required_nodes`.
bool FSExactOIT::growNodePool(U32 required_nodes)
{
    constexpr U64 node_bytes = 32;
    const U32 grown_capacity = computeGrownCapacity(required_nodes, sResources.capacity);

    if (grown_capacity > sResources.capacity)
    {
        while (glGetError() != GL_NO_ERROR) {}
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.nodes);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     GLsizeiptr(static_cast<U64>(grown_capacity) * node_bytes), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (glGetError() == GL_NO_ERROR)
        {
            sResources.capacity = grown_capacity;
            LL_INFOS("ExactOIT") << "Grew exact OIT node capacity to " << grown_capacity << LL_ENDL;
        }
        else
        {
            sResources.available = false;
            LL_WARNS("ExactOIT") << "Unable to grow exact OIT node buffer; exact OIT disabled for this session."
                << LL_ENDL;
        }
    }
    return sResources.capacity >= required_nodes;
}

// Handles overflow failure and buffer-growth policy; returns true when vanilla fallback is required.
bool FSExactOIT::captureOverflowed(U32 required_nodes, U32 overflow_flag)
{
    if (overflow_flag == 0 && required_nodes <= sResources.capacity)
    {
        return false;
    }

    ++sResources.overflowCount;
    LL_WARNS_ONCE("ExactOIT") << "Exact OIT node capacity exceeded (required "
        << required_nodes << ", capacity " << sResources.capacity
        << "); rendering complete vanilla transparency for this frame." << LL_ENDL;
    discardCapture();

    // Defer the actual glBufferData reallocation to next frame's
    // beginFrame(), before any capture or speculative sort work has touched
    // the buffer this frame -- see pendingGrowthNodes. Predict success from
    // the policy alone (no GL call) to decide the skip-frame policy now.
    const U32 grown_capacity = computeGrownCapacity(required_nodes, sResources.capacity);
    const bool will_grow = grown_capacity > sResources.capacity;
    if (will_grow)
    {
        sResources.pendingGrowthNodes = llmax(sResources.pendingGrowthNodes, required_nodes);
    }
    if (!will_grow)
    {
        // Demand already exceeds the safe VRAM cap: growth is impossible this
        // session. Skip capture for a growing number of frames instead of
        // re-attempting (and re-discarding) every frame.
        sResources.skipFramesRemaining = llmin(2u << sResources.consecutiveOverflowsAtCap, 60u);
        ++sResources.consecutiveOverflowsAtCap;
        static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderExactOITDebugMode", 0);
        if (debug_mode == 0)
        {
            LL_INFOS("ExactOIT") << "Exact OIT demand exceeds safe capacity; skipping capture for "
                << sResources.skipFramesRemaining << " frames." << LL_ENDL;
        }
    }
    return true;
}

// Updates peak statistics and emits bounded camera-transition diagnostics.
void FSExactOIT::recordCaptureStats(U32 nodes, U32 maximum_list, bool mouselook)
{
    sResources.peakNodes = llmax(sResources.peakNodes, nodes);
    static bool initialized = false;
    static bool was_mouselook = false;
    static U32 samples_remaining = 0;
    static U32 peak_nodes = 0;
    static U32 peak_list = 0;

    if (!initialized)
    {
        initialized = true;
        was_mouselook = mouselook;
    }
    else if (mouselook != was_mouselook)
    {
        was_mouselook = mouselook;
        samples_remaining = 30;
        peak_nodes = nodes;
        peak_list = maximum_list;
        LL_INFOS("ExactOIT") << "Camera mode changed to "
            << (mouselook ? "first person" : "third person")
            << "; capture nodes " << nodes << ", maximum pixel list " << maximum_list
            << ", capacity " << sResources.capacity << LL_ENDL;
    }
    if (samples_remaining > 0)
    {
        peak_nodes = llmax(peak_nodes, nodes);
        peak_list = llmax(peak_list, maximum_list);
        if (--samples_remaining == 0)
        {
            LL_INFOS("ExactOIT") << "Camera transition settled after 30 Exact OIT frames; peak nodes "
                << peak_nodes << ", peak pixel list " << peak_list << LL_ENDL;
        }
    }
}

// Reports whether Exact OIT has nothing to composite this frame (mode off,
// no capture ran, or the shader/resource set is unavailable). When this
// returns false, waitValidation() must still be called to consume the fence.
bool FSExactOIT::captureInactive(bool cube_snapshot, bool impostor_render)
{
    return !isEnabled() || cube_snapshot || impostor_render || !sResources.available ||
        !gExactOITCompositeProgram.mProgramObject || !sCaptureCompleted;
}

// Issues the memory barrier that makes captured SSBO/image writes visible to
// subsequent draws. Must run before sort pass 1 is issued, and before the
// fence wait in waitValidation().
void FSExactOIT::beginValidation()
{
    // No atomic counter buffers are used by Exact OIT (only SSBO/image
    // atomics), so GL_ATOMIC_COUNTER_BARRIER_BIT is unnecessary here.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

// Waits only until the capture draws finish (not the whole pipeline), reads
// back the control words, and applies overflow/growth policy. Sort pass 1
// (which does not depend on this readback) should already be in flight.
FSExactOIT::ValidationResult FSExactOIT::waitValidation(bool mouselook, U32& maximum_list)
{
    maximum_list = 0;
    if (sResources.captureFence)
    {
        LL_PROFILE_ZONE_NAMED("Exact OIT fence wait");
        GLenum result;
        bool first_call = true;
        do
        {
            result = glClientWaitSync(sResources.captureFence,
                first_call ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 1000000000ull);
            first_call = false;
        }
        while (result == GL_TIMEOUT_EXPIRED);
        glDeleteSync(sResources.captureFence);
        sResources.captureFence = 0;
    }

    U32 control[4] = {};
    if (sResources.readbackMapped)
    {
        // One slot suffices: the CPU reads it here before the next frame's
        // GPU-side copy is issued, so there is no write-after-read hazard.
        LL_PROFILE_ZONE_NAMED("Exact OIT mapped readback read");
        memcpy(control, sResources.readbackMapped, sizeof(control));
    }
    else
    {
        LL_PROFILE_ZONE_NAMED("Exact OIT validation readback");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(control), control);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    maximum_list = control[3];
    recordCaptureStats(control[0], control[3], mouselook);
    sResources.lastRequiredNodes = control[0];
    if (captureOverflowed(control[0], control[2]))
    {
        return ValidationResult::FALLBACK_REQUIRED;
    }

    // Not overflowed this frame: reset the escalating skip policy and, if
    // demand is approaching capacity, request growth for next frame rather
    // than waiting for an overflow (and a double-render) frame later. The
    // actual reallocation is deferred to beginFrame() (see
    // pendingGrowthNodes): this frame's capture data is already live in
    // sResources.nodes and, with the speculative pass (E4), a sort pass may
    // already be queued against it -- reallocating here would race that
    // in-flight GPU work.
    sResources.consecutiveOverflowsAtCap = 0;
    if (control[0] > sResources.capacity * 3 / 4 && sResources.capacity < U32(safeNodeCapacity()))
    {
        // computeGrownCapacity() already applies its own headroom/doubling
        // policy on top of this; doubling here too jumped a 29M-node demand
        // straight to the 2GB safe cap.
        sResources.pendingGrowthNodes = llmax(sResources.pendingGrowthNodes, control[0]);
    }
    return ValidationResult::COMPLETE;
}

// Binds the captured images and shader-storage buffers required by composite passes.
void FSExactOIT::bindCompositeResources()
{
    glBindImageTexture(0, sResources.heads, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(1, sResources.counts, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sResources.nodes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sResources.control);
}

// Clears one GPU-generated indirect-dispatch count while preserving its 1,1 dimensions.
static void clearSortQueueCount(GLuint queue)
{
    const U32 zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, queue);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, sizeof(U32),
                         GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

// Sorts captured lists through compact compute queues, returning false when unavailable.
bool FSExactOIT::sortWithCompute(U32 width, U32 height, U32 maximum_list)
{
    static LLCachedControl<bool> compute_sort(gSavedSettings, "RenderExactOITComputeSort", false);
    if (!compute_sort || !sResources.computeSortAvailable || maximum_list <= 1u)
    {
        return false;
    }

    bindCompositeResources();
    clearSortQueueCount(sResources.sortQueues[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sResources.sortQueues[0]);

    {
        LL_PROFILE_GPU_ZONE("Exact OIT compute classify");
        gExactOITClassifyProgram.bind();
        glDispatchCompute((width + 15u) / 16u, (height + 15u) / 16u, 1u);
        gExactOITClassifyProgram.unbind();
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_COMMAND_BARRIER_BIT);

    clearSortQueueCount(sResources.sortQueues[1]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sResources.sortQueues[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sResources.sortQueues[1]);
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.sortQueues[0]);
    {
        LL_PROFILE_GPU_ZONE("Exact OIT compute block sort");
        static LLCachedControl<bool> opaque_cutoff(
            gSavedSettings, "RenderExactOITOpaqueCutoff", true);
        static LLStaticHashedString oit_opaque_cutoff("oitOpaqueCutoff");
        gExactOITBlockSortProgram.bind();
        gExactOITBlockSortProgram.uniform1i(oit_opaque_cutoff, opaque_cutoff);
        glDispatchComputeIndirect(0);
        gExactOITBlockSortProgram.unbind();
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_COMMAND_BARRIER_BIT);

    U32 input_queue = 1u;
    U32 output_queue = 0u;
    for (U32 sorted_width = 64u; sorted_width < maximum_list; sorted_width <<= 1u)
    {
        LL_PROFILE_GPU_ZONE("Exact OIT compute deep merge");
        clearSortQueueCount(sResources.sortQueues[output_queue]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sResources.sortQueues[input_queue]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sResources.sortQueues[output_queue]);
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, sResources.sortQueues[input_queue]);
        gExactOITMergeProgram.bind();
        glDispatchComputeIndirect(0);
        gExactOITMergeProgram.unbind();
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_COMMAND_BARRIER_BIT);
        std::swap(input_queue, output_queue);
    }
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
}

// Copies the untouched opaque screen color into the composite background texture.
void FSExactOIT::copyOpaqueScene(LLRenderTarget& screen)
{
    LL_PROFILE_GPU_ZONE("Exact OIT opaque copy");
    glCopyImageSubData(screen.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       sOpaqueTarget.getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       screen.getWidth(), screen.getHeight(), 1);
}

// E6: chunk size take_run() sorts in registers before falling back to
// natural-run detection. Must match OIT_CHUNK in exactOITCompositeF.glsl.
constexpr U32 OIT_CHUNK = 16u;

// Sorts every captured pixel list and blends the exact result over the opaque scene.
// `sort_pass_1_issued` is true when finishFrame() already ran the fragment
// sorter's first pass speculatively (overlapped with the capture fence wait);
// this then resumes from the second pass instead of redoing the first.
// `shallow_limit` is E5's K, computed once in finishFrame() and shared with
// the speculative pass issued there; it must not be recomputed here.
void FSExactOIT::composite(LLRenderTarget& screen, LLVertexBuffer& screen_triangle, U32 maximum_list,
                           bool sort_pass_1_issued, U32 shallow_limit)
{
    copyOpaqueScene(screen);
    LLGLDisable blend(GL_BLEND);
    bindCompositeResources();

    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderExactOITDebugMode", 0);
    static LLCachedControl<bool> opaque_cutoff(gSavedSettings, "RenderExactOITOpaqueCutoff", true);
    static LLStaticHashedString oit_debug_mode("oitDebugMode");
    static LLStaticHashedString oit_pass("oitPass");
    static LLStaticHashedString oit_compute_sort_active("oitComputeSortActive");
    // Limit opaque-cutoff discovery to the first natural-sort invocation.
    static LLStaticHashedString oit_first_sort_pass("oitFirstSortPass");
    static LLStaticHashedString oit_shallow_limit("oitShallowLimit");
    static LLStaticHashedString oit_opaque_cutoff("oitOpaqueCutoff");
    // <AS:Chanayane> Self-lighting floater isolate-background mode: see the
    // dedicated pass 3 block below and asbackgroundisolate.h for the full
    // story.
    const bool isolate_write_depth = ASBackgroundIsolate::isActive();
    // </AS:Chanayane>

    bool used_compute_sort = false;
    U32 sort_passes = 0;
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.setColorMask(false, false);
        used_compute_sort = !sort_pass_1_issued &&
            sortWithCompute(screen.getWidth(), screen.getHeight(), maximum_list);
        if (!used_compute_sort)
        {
            gExactOITCompositeProgram.bind();
            gExactOITCompositeProgram.uniform1i(oit_debug_mode, debug_mode);
            gExactOITCompositeProgram.uniform1i(oit_shallow_limit, (S32)shallow_limit);
            gExactOITCompositeProgram.uniform1i(oit_opaque_cutoff, opaque_cutoff);
            screen_triangle.setBuffer();
            // E5: pass 1 (and thus any further merge rounds) is skipped
            // entirely when every pixel's raw count could be at most
            // max(K, 1) -- i.e. nothing on screen needs a fullscreen sort
            // pass at all. The speculative pass in finishFrame(), if issued,
            // already ran with this same shallow_limit and is a no-op per
            // pixel in that case, so skipping the remaining rounds here is
            // still correct.
            if (maximum_list > llmax(shallow_limit, 1u))
            {
                LL_PROFILE_GPU_ZONE("Exact OIT natural sort");
                gExactOITCompositeProgram.uniform1i(oit_pass, 1);
                // E6: take_run() chunks natural runs shorter than OIT_CHUNK,
                // so after the first round every run has >= OIT_CHUNK nodes
                // except the last: runs <= ceil(maximum_list / OIT_CHUNK),
                // and each further round halves the run count.
                const U32 runs = (maximum_list + OIT_CHUNK - 1u) / OIT_CHUNK;
                U32 total_passes = 1u;
                while ((1u << total_passes) < runs) ++total_passes;
                // Pass 1 (oitPass == 1) never reads oitDebugMode, only
                // oitFirstSortPass/oitShallowLimit/oitOpaqueCutoff, so the
                // speculative pass in finishFrame() is a bit-identical
                // stand-in for round 1 here regardless of debug mode; skip
                // straight to round 2 when it ran.
                const U32 start_round = sort_pass_1_issued ? 2u : 1u;
                for (U32 round = start_round; round <= total_passes; ++round)
                {
                    LL_PROFILE_GPU_ZONE("Exact OIT natural sort pass");
                    // Prune fully hidden nodes before the first merge pass.
                    gExactOITCompositeProgram.uniform1i(oit_first_sort_pass,
                                                        opaque_cutoff && round == 1u);
                    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
                    ++sort_passes;
                }
            }
            gExactOITCompositeProgram.unbind();
        }
    }

    // Phase-0 measurement: objective, bounded periodic snapshot of demand vs.
    // capacity and sort cost, for comparing RenderOITMode settings and scenes.
    if (debug_mode != 0)
    {
        static U32 frame_counter = 0;
        if (++frame_counter >= 300)
        {
            frame_counter = 0;
            const U32 total_sort_passes = sort_passes + (used_compute_sort ? 0 : (sort_pass_1_issued ? 1 : 0));
            LL_INFOS("ExactOIT") << "Nodes used " << sResources.lastRequiredNodes
                << " / capacity " << sResources.capacity
                << ", max pixel list " << maximum_list
                << ", sort passes " << (used_compute_sort ? 0 : total_sort_passes)
                << (used_compute_sort ? " (compute sort)" : "") << LL_ENDL;
        }
    }

    {
        LL_PROFILE_GPU_ZONE("Exact OIT final blend");
        LLGLDepthTest depth(GL_FALSE);
        gGL.setColorMask(true, true);
        gExactOITCompositeProgram.bind();
        gExactOITCompositeProgram.uniform1i(oit_debug_mode, debug_mode);
        gExactOITCompositeProgram.uniform1i(oit_compute_sort_active, used_compute_sort);
        gExactOITCompositeProgram.uniform1i(oit_pass, 2);
        // Pass 2 reads oitShallowLimit/oitOpaqueCutoff unconditionally
        // (blend_shallow() dispatch); set them here too, since the fragment
        // sort loop above -- the only other place that sets them this frame
        // -- is skipped whenever the compute sorter ran or every pixel was
        // shallow. A stale value from a previous bind of this program object
        // would silently desync pass 1's OIT_SORTED flag from pass 2's
        // dispatch.
        gExactOITCompositeProgram.uniform1i(oit_shallow_limit, (S32)shallow_limit);
        gExactOITCompositeProgram.uniform1i(oit_opaque_cutoff, opaque_cutoff);
        gExactOITCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE,
                                              &sOpaqueTarget, false, LLTexUnit::TFO_POINT, 0);
        screen_triangle.setBuffer();
        screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
        gExactOITCompositeProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        gExactOITCompositeProgram.unbind();
    }

    // <AS:Chanayane> Self-lighting floater isolate-background mode: a
    // separate depth-only re-pass, run only while isolate mode is active,
    // AFTER the color blend above has fully completed. It discards on every
    // pixel with no real captured OIT coverage and writes a near-plane
    // depth everywhere else (see the oitPass == 3 branch in
    // exactOITCompositeF.glsl), so a later depth-tested isolate backdrop
    // pass correctly treats OIT-composited content as occupied instead of
    // painting over it. Color writes are masked off for this pass -- it
    // only ever touches depth. Doing this as a fully separate pass (rather
    // than writing depth inline during the color blend) avoids ever
    // needing to read the scene's existing depth while that same
    // attachment is simultaneously bound for writing, which is undefined
    // behavior.
    if (isolate_write_depth)
    {
        LL_PROFILE_GPU_ZONE("Exact OIT isolate depth");
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
        gGL.setColorMask(false, false);
        gExactOITCompositeProgram.bind();
        gExactOITCompositeProgram.uniform1i(oit_pass, 3);
        screen_triangle.setBuffer();
        screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
        gExactOITCompositeProgram.unbind();
        gGL.setColorMask(true, true);
    }
    // </AS:Chanayane>

    static bool previous_requested = false;
    static bool previous_available = false;
    static bool previous_used = false;
    static LLCachedControl<bool> requested_control(gSavedSettings, "RenderExactOITComputeSort", false);
    const bool requested = requested_control;
    if (requested != previous_requested ||
        sResources.computeSortAvailable != previous_available ||
        used_compute_sort != previous_used)
    {
        LL_INFOS("ExactOIT") << "Compute sort requested " << requested
                             << ", available " << sResources.computeSortAvailable
                             << ", used this frame " << used_compute_sort
                             << ", maximum list " << maximum_list << LL_ENDL;
        previous_requested = requested;
        previous_available = sResources.computeSortAvailable;
        previous_used = used_compute_sort;
    }
}

// Issues the first fragment natural-merge sort pass speculatively, before the
// overflow/capacity decision is known. Safe even for an eventually-discarded
// (overflowed) capture: allocation failure returns before a node is linked,
// so this pass never follows a pointer to an unwritten node. Its GPU work
// then overlaps the fence wait in waitValidation() instead of running after.
// Only used for the fragment sorter; the compute-sort path still needs
// maximum_list up front for its dispatch counts and is issued after the wait.
// `shallow_limit` must equal the K composite() uses this same frame (see the
// E4/E5 interaction note in the performance plan): a mismatch would make
// this pass sort pixels composite() then treats as unsorted, or skip pixels
// composite() treats as sorted.
static void issueSpeculativeFirstSortPass(LLVertexBuffer& screen_triangle, U32 shallow_limit)
{
    static LLCachedControl<bool> opaque_cutoff(gSavedSettings, "RenderExactOITOpaqueCutoff", true);
    static LLStaticHashedString oit_pass("oitPass");
    static LLStaticHashedString oit_first_sort_pass("oitFirstSortPass");
    static LLStaticHashedString oit_shallow_limit("oitShallowLimit");
    static LLStaticHashedString oit_opaque_cutoff("oitOpaqueCutoff");

    LL_PROFILE_GPU_ZONE("Exact OIT speculative sort pass");
    LLGLDepthTest depth(GL_FALSE);
    gGL.setColorMask(false, false);
    gExactOITCompositeProgram.bind();
    // oitPass == 1 never reads oitDebugMode (see exactOITCompositeF.glsl),
    // so it is left at whatever value the program object last had bound.
    gExactOITCompositeProgram.uniform1i(oit_pass, 1);
    // Equivalent to composite()'s (opaque_cutoff && width == 1) test: this
    // speculative pass always is width == 1.
    gExactOITCompositeProgram.uniform1i(oit_first_sort_pass, opaque_cutoff);
    gExactOITCompositeProgram.uniform1i(oit_shallow_limit, (S32)shallow_limit);
    gExactOITCompositeProgram.uniform1i(oit_opaque_cutoff, opaque_cutoff);
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gExactOITCompositeProgram.unbind();
    gGL.setColorMask(true, true);
}

// Validates the frame, performs complete fallback or composite, and dispatches debug alpha.
void FSExactOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen,
                             LLVertexBuffer& screen_triangle, bool cube_snapshot,
                             bool impostor_render, bool mouselook)
{
    if (captureInactive(cube_snapshot, impostor_render))
    {
        return;
    }

    beginValidation();

    // Shallow-list limit K (E5): 16 in normal mode, 0 (disabled, i.e. every
    // diagnostic sees the fully-sorted path) in any debug mode. Computed
    // once here and passed to both the speculative pass below and
    // composite(): they must agree, or the speculative pass would sort
    // pixels composite() then treats as unsorted, or skip pixels composite()
    // treats as sorted.
    static LLCachedControl<S32> debug_mode(gSavedSettings, "RenderExactOITDebugMode", 0);
    const U32 shallow_limit = debug_mode == 0 ? 16u : 0u;

    // The compute sorter needs maximum_list up front for its dispatch counts,
    // so it cannot be issued speculatively; only the fragment path overlaps
    // the fence wait below with GPU work. This mirrors sortWithCompute()'s own
    // gating (setting on AND resources available), computed early so both
    // this speculative issue and the later composite() agree on which sorter
    // will actually run this frame.
    static LLCachedControl<bool> compute_sort_requested(gSavedSettings, "RenderExactOITComputeSort", false);
    const bool will_use_compute_sort = compute_sort_requested && sResources.computeSortAvailable;
    const bool sort_pass_1_issued = !will_use_compute_sort;
    if (sort_pass_1_issued)
    {
        // Explicit bind: do not rely on prepareCaptureBuffers() having left
        // the same image/SSBO units bound from capture; composite()'s own
        // bindCompositeResources() call below is then a harmless repeat.
        bindCompositeResources();
        issueSpeculativeFirstSortPass(screen_triangle, shallow_limit);
    }

    U32 maximum_list = 0;
    const ValidationResult validation = waitValidation(mouselook, maximum_list);
    if (validation == ValidationResult::FALLBACK_REQUIRED)
    {
        // Sort pass 1, if issued, mutated list heads/counts for a capture
        // that is now discarded; the vanilla fallback below never reads
        // those images, so no cleanup is required.
        LL_PROFILE_GPU_ZONE("Exact OIT composite");
        VanillaFallbackScope fallback_scope;
        for (LLDrawPool* pool : pipeline.mPools)
        {
            if (pool->getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
            {
                LLVertexBuffer::unbind();
                pool->beginPostDeferredPass(0);
                pool->renderPostDeferred(0);
                pool->endPostDeferredPass(0);
            }
        }
        return;
    }

    LL_PROFILE_GPU_ZONE("Exact OIT composite");
    composite(screen, screen_triangle, maximum_list, sort_pass_1_issued, shallow_limit);
    // The viewer's Highlight Transparent overlay is drawn after compositing and
    // would obscure Exact OIT diagnostic colors.
    if (debug_mode == 0)
    {
        for (LLDrawPool* pool : pipeline.mPools)
        {
            if (pool->getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
            {
                static_cast<LLDrawPoolAlpha*>(pool)->renderDebugAlpha();
                break;
            }
        }
    }
}

// Releases viewport resources and optionally preserves the large reusable node pool.
void FSExactOIT::releaseResources(bool preserve_node_pool)
{
    sOpaqueTarget.release();
    if (sResources.heads)
    {
        glDeleteTextures(1, &sResources.heads);
    }
    if (sResources.counts)
    {
        glDeleteTextures(1, &sResources.counts);
    }
    if (sResources.headFBO)
    {
        glDeleteFramebuffers(1, &sResources.headFBO);
    }
    if (!preserve_node_pool && sResources.nodes)
    {
        glDeleteBuffers(1, &sResources.nodes);
    }
    if (sResources.captureFence)
    {
        glDeleteSync(sResources.captureFence);
        sResources.captureFence = 0;
    }
    if (sResources.control)
    {
        glDeleteBuffers(1, &sResources.control);
    }
    if (sResources.readback)
    {
        if (sResources.readbackMapped)
        {
            glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            sResources.readbackMapped = nullptr;
        }
        glDeleteBuffers(1, &sResources.readback);
    }
    glDeleteBuffers(2, sResources.sortQueues);

    sResources.heads = 0;
    sResources.counts = 0;
    sResources.headFBO = 0;
    sResources.control = 0;
    sResources.readback = 0;
    sResources.sortQueues[0] = 0;
    sResources.sortQueues[1] = 0;
    sResources.sortQueueCapacity = 0;
    sResources.computeSortAvailable = false;
    sResources.pendingGrowthNodes = 0;
    if (!preserve_node_pool)
    {
        sResources.nodes = 0;
        sResources.capacity = 0;
    }
    sResources.available = false;
}

// Requests that the next screen-buffer release retain the node pool across resizing.
void FSExactOIT::retainNodePoolOnNextRelease()
{
    sRetainNodePoolOnRelease = true;
}

// Releases resources using and then clearing the pending node-pool retention request.
void FSExactOIT::releaseResources()
{
    const bool preserve_node_pool = sRetainNodePoolOnRelease;
    sRetainNodePoolOnRelease = false;
    releaseResources(preserve_node_pool);
}

// Resets availability and failure state before attempting a fresh allocation.
void FSExactOIT::prepareResourceAllocation()
{
    const bool enabled = isEnabled();
    releaseResources(enabled);

    if (!enabled)
    {
        sResources.peakNodes = 0;
    }
}

// Validates allocation prerequisites and returns whether viewport allocation may proceed.
bool FSExactOIT::beginResourceAllocation(U32 width, U32 height)
{
    prepareResourceAllocation();
    return isEnabled() && sOpaqueTarget.allocate(width, height, GL_RGBA16F);
}

// Allocates the head/count images and their framebuffer, returning completeness status.
bool FSExactOIT::allocateCaptureImages(U32 width, U32 height)
{
    glGenTextures(1, &sResources.heads);
    glBindTexture(GL_TEXTURE_2D, sResources.heads);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, width, height);

    glGenTextures(1, &sResources.counts);
    glBindTexture(GL_TEXTURE_2D, sResources.counts);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, width, height);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &sResources.headFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, sResources.headFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sResources.heads, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sResources.counts, 0);
    const GLenum clear_buffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, clear_buffers);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    return complete;
}

// Allocates or reuses the bounded node pool and creates its control buffer.
void FSExactOIT::allocateNodePool(U32 width, U32 height, bool capture_images_ready)
{
    constexpr U64 node_bytes = 32;
    const U64 vram_bytes = static_cast<U64>(gGLManager.mVRAM) * 1024u * 1024u;
    const U64 safe_bytes = llmin<U64>(vram_bytes / 4u, 2ull * 1024ull * 1024ull * 1024ull);
    const U64 wanted_nodes = static_cast<U64>(width) * static_cast<U64>(height) * 4ull;
    const U64 safe_nodes = safe_bytes / node_bytes;
    const U32 requested_capacity = U32(llmin<U64>(wanted_nodes, llmin<U64>(safe_nodes, 0xfffffffeull)));
    const U32 retained_capacity = U32(llmin<U64>(sResources.capacity, llmin<U64>(safe_nodes, 0xfffffffeull)));
    const U32 allocation_capacity = llmax(requested_capacity, retained_capacity);

    if (!capture_images_ready || allocation_capacity == 0)
    {
        return;
    }

    while (glGetError() != GL_NO_ERROR) {}
    if (!sResources.nodes)
    {
        glGenBuffers(1, &sResources.nodes);
    }
    if (allocation_capacity > sResources.capacity)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.nodes);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     GLsizeiptr(static_cast<U64>(allocation_capacity) * node_bytes), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        sResources.capacity = allocation_capacity;
    }

    glGenBuffers(1, &sResources.control);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
    const U32 control[4] = { 0, sResources.capacity, 0, 0 };
    // Device-local: every captured fragment hits this buffer with atomics
    // (atomicAdd/atomicMax/atomicOr). It must never be host-mapped — a
    // MAP_READ | PERSISTENT | COHERENT buffer is allocated in system memory,
    // which would put every capture fragment's atomics on the PCIe bus
    // (measured ~200ms stall in a 1.5M-node sprite scene; see
    // doc/ayanestorm-oit-e4-fence-stall-question.md).
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(control), control, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Separate 16-byte host-visible copy of the control words, filled by a
    // GPU-side glCopyBufferSubData after capture. Only this buffer is
    // persistently mapped; the CPU never touches the control buffer above.
    sResources.readback = 0;
    sResources.readbackMapped = nullptr;
    if (gGLManager.mGLVersion >= 4.39f && glBufferStorage && glMapBufferRange)
    {
        const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glGenBuffers(1, &sResources.readback);
        glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
        glBufferStorage(GL_COPY_WRITE_BUFFER, sizeof(control), nullptr, flags);
        sResources.readbackMapped = static_cast<U32*>(
            glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, sizeof(control), flags));
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        if (!sResources.readbackMapped)
        {
            glDeleteBuffers(1, &sResources.readback);
            sResources.readback = 0;
        }
    }
    sResources.available = glGetError() == GL_NO_ERROR;
}

// Allocates two packed-pixel queues with indirect-dispatch headers.
void FSExactOIT::allocateComputeSortQueues(U32 width, U32 height)
{
    sResources.computeSortAvailable = false;
    if (!sResources.available ||
        !gExactOITClassifyProgram.mProgramObject ||
        !gExactOITBlockSortProgram.mProgramObject ||
        !gExactOITMergeProgram.mProgramObject ||
        width > 0xffffu || height > 0xffffu)
    {
        return;
    }

    const U64 pixel_count = static_cast<U64>(width) * static_cast<U64>(height);
    if (pixel_count == 0u || pixel_count > 0xffffffffu)
    {
        return;
    }

    const U64 queue_bytes = 4ull * sizeof(U32) + pixel_count * sizeof(U32);
    const U32 header[4] = { 0u, 1u, 1u, 0u };
    while (glGetError() != GL_NO_ERROR) {}
    glGenBuffers(2, sResources.sortQueues);
    for (GLuint queue : sResources.sortQueues)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, queue);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(queue_bytes), nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(header), header);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    sResources.sortQueueCapacity = static_cast<U32>(pixel_count);
    sResources.computeSortAvailable = glGetError() == GL_NO_ERROR;
    if (!sResources.computeSortAvailable)
    {
        glDeleteBuffers(2, sResources.sortQueues);
        sResources.sortQueues[0] = 0;
        sResources.sortQueues[1] = 0;
        sResources.sortQueueCapacity = 0;
    }
}

// Allocates all setting-dependent Exact OIT resources for the current viewport.
void FSExactOIT::allocateResources(U32 width, U32 height)
{
    if (isEnabled())
    {
        sRuntimeAllocationAttempted = true;
    }
    if (!beginResourceAllocation(width, height))
    {
        return;
    }

    allocateNodePool(width, height, allocateCaptureImages(width, height));
    allocateComputeSortQueues(width, height);
}

#endif // LL_DARWIN
