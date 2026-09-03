/**
 * @file fsexactoit.h
 * @brief Firestorm Exact OIT integration.
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

#ifndef FS_EXACT_OIT_H
#define FS_EXACT_OIT_H

#include <vector>

#include "llglslshader.h"
#include "llmaterial.h"

class LLSD;
class LLDrawPoolAlpha;
class LLDrawInfo;
class LLPipeline;
class LLRenderTarget;
class LLVertexBuffer;

class FSExactOIT
{
public:
    static const char* shaderCacheRevision();
    static bool loadShaders(bool success, S32 shader_level, bool use_sun_shadow,
                            bool gltf_enabled, std::vector<LLGLSLShader*>& shader_list);
    static void registerShaders(std::vector<LLGLSLShader*>& shader_list);
    static void unloadShaders();
    static void appendDiagnostics(LLSD& info);
    using PrepareShader = void (*)(LLGLSLShader*, bool, F32);
    static bool renderPostDeferredCapture(LLDrawPoolAlpha& pool, PrepareShader prepare,
                                          F32 water_sign, LLGLSLShader*& emissive_shader,
                                          LLGLSLShader*& pbr_emissive_shader);
    static void finishFrame(LLPipeline& pipeline, LLRenderTarget& screen,
                            LLVertexBuffer& screen_triangle, bool cube_snapshot,
                            bool impostor_render, bool mouselook);

    static void beginFrame();
    static bool captureCompleted();
    static bool captureActive();
    static bool configureCapturedDrawIfActive(LLGLSLShader* shader, U32 color_source,
                                              U32 color_destination, U32 alpha_source,
                                              U32 alpha_destination);
    static bool handleCapturedEmissives(LLDrawPoolAlpha& pool, bool depth_only,
                                        std::vector<LLDrawInfo*>& emissives,
                                        std::vector<LLDrawInfo*>& pbr_emissives,
                                        std::vector<LLDrawInfo*>& rigged_emissives,
                                        std::vector<LLDrawInfo*>& pbr_rigged_emissives);
    static void configureGLTFCapturedDraw(LLGLSLShader& shader);
    static LLGLSLShader& gltfProgram(LLGLSLShader& ordinary_program);
    static LLGLSLShader* alphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* pbrAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* fullbrightAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* materialAlphaShader(U32 mask, LLGLSLShader* ordinary);
    static void retainNodePoolOnNextRelease();
    static void releaseResources();
    static void allocateResources(U32 width, U32 height);
    // User intent AND hardware support, mirroring FSAVBOIT::requested(). Public
    // so the neutral dispatcher can publish the selected transparency mode
    // without reaching into either renderer's internals.
    static bool isEnabled();

private:
    enum class ValidationResult { INACTIVE, COMPLETE, FALLBACK_REQUIRED };
    class VanillaFallbackScope
    {
    public:
        VanillaFallbackScope();
        ~VanillaFallbackScope();
        VanillaFallbackScope(const VanillaFallbackScope&) = delete;
        VanillaFallbackScope& operator=(const VanillaFallbackScope&) = delete;
    };
    class CaptureScope
    {
    public:
        CaptureScope();
        ~CaptureScope();
        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;
    };
    static bool isSupported();
    static bool loadGLTFShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadPBRGlowShaders(S32 shader_level);
    static bool loadEmissiveShaders(S32 shader_level);
    static bool loadCompositeShader(S32 shader_level);
    static bool loadAlphaShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadPBRAlphaShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadFullbrightAlphaShaders(S32 shader_level);
    static bool loadMaterialAlphaShaders(S32 shader_level, bool use_sun_shadow,
                                         std::vector<LLGLSLShader*>& shader_list);
    static void loadComputeSortShaders(S32 shader_level);
    static bool shadersReady();
    static void discardCapture();
    static void setVanillaFallback(bool active);
    static void beginCapture();
    static void endCapture();
    static void markCaptureCompleted();
    static void prepareCaptureBuffers();
    static bool captureEligible(bool rendering_huds, bool impostor_render, bool cube_snapshot,
                                U32 width, U32 height);
    static void prepareCaptureShaders(PrepareShader prepare, F32 water_sign);
    static LLGLSLShader* emissiveShader();
    static LLGLSLShader* pbrGlowShader();
    static bool captureInactive(bool cube_snapshot, bool impostor_render);
    static void beginValidation();
    static ValidationResult waitValidation(bool mouselook, U32& maximum_list);
    static void composite(LLRenderTarget& screen, LLVertexBuffer& screen_triangle, U32 maximum_list,
                          bool sort_pass_1_issued);
    static bool sortWithCompute(U32 width, U32 height, U32 maximum_list);
    static void releaseResources(bool preserve_node_pool);
    struct Resources
    {
        GLuint heads = 0;
        GLuint counts = 0;
        GLuint headFBO = 0;
        GLuint nodes = 0;
        GLuint control = 0;
        GLuint readback = 0;          // 16-byte host-visible copy of the control words
        U32* readbackMapped = nullptr;
        GLsync captureFence = 0;
        GLuint sortQueues[2] = {};
        U32 capacity = 0;
        U32 sortQueueCapacity = 0;
        U32 peakNodes = 0;
        U32 overflowCount = 0;
        U32 lastRequiredNodes = 0;
        U32 skipFramesRemaining = 0;
        U32 consecutiveOverflowsAtCap = 0;
        bool computeSortAvailable = false;
        bool available = false;
    };
    static bool captureOverflowed(U32 required_nodes, U32 overflow_flag);
    static bool growNodePool(U32 required_nodes);
    static void recordCaptureStats(U32 nodes, U32 maximum_list, bool mouselook);
    static void bindCompositeResources();
    static void copyOpaqueScene(LLRenderTarget& screen);
    static void prepareResourceAllocation();
    static bool beginResourceAllocation(U32 width, U32 height);
    static bool allocateCaptureImages(U32 width, U32 height);
    static void allocateNodePool(U32 width, U32 height, bool capture_images_ready);
    static void allocateComputeSortQueues(U32 width, U32 height);
    static bool sCaptureCompleted;
    static bool sCaptureClearNeeded;
    static bool sVanillaFallbackActive;
    static bool sCaptureActive;
    static bool sRuntimeAllocationAttempted;
    static bool sRetainNodePoolOnRelease;
    static LLRenderTarget sOpaqueTarget;
    static Resources sResources;
};

#endif // FS_EXACT_OIT_H
