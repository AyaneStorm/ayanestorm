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
class LLRenderTarget;
class LLVertexBuffer;

class FSExactOIT
{
public:
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
    static const char* shaderCacheRevision();
    static bool loadShaders(bool success, S32 shader_level, bool use_sun_shadow,
                            bool gltf_enabled, std::vector<LLGLSLShader*>& shader_list);
    static void registerShaders(std::vector<LLGLSLShader*>& shader_list);
    static void unloadShaders();
    static void appendDiagnostics(LLSD& info);

    static void beginFrame();
    static bool captureCompleted();
    static bool vanillaFallbackActive();
    static bool captureActive();
    static void markCaptureCompleted();
    static void prepareCaptureBuffers();
    static bool captureEligible(bool rendering_huds, bool impostor_render, bool cube_snapshot,
                                U32 width, U32 height);
    using PrepareShader = void (*)(LLGLSLShader*, bool, F32);
    static void prepareCaptureShaders(PrepareShader prepare, F32 water_sign);
    static void configureCapturedDraw(LLGLSLShader& shader, U32 color_source,
                                      U32 color_destination, U32 alpha_source,
                                      U32 alpha_destination);
    static void configureGLTFCapturedDraw(LLGLSLShader& shader);
    static LLGLSLShader& gltfProgram(LLGLSLShader& ordinary_program);
    static LLGLSLShader* alphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* pbrAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* fullbrightAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* materialAlphaShader(U32 mask, LLGLSLShader* ordinary);
    static LLGLSLShader* emissiveShader();
    static LLGLSLShader* pbrGlowShader();
    static ValidationResult validateCapture(bool cube_snapshot, bool impostor_render,
                                            bool mouselook, U32& maximum_list);
    static void composite(LLRenderTarget& screen, LLVertexBuffer& screen_triangle, U32 maximum_list);
    static void retainNodePoolOnNextRelease();
    static void releaseResources();
    static void allocateResources(U32 width, U32 height);

private:
    static bool isSupported();
    static bool isEnabled();
    static bool loadGLTFShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadPBRGlowShaders(S32 shader_level);
    static bool loadEmissiveShaders(S32 shader_level);
    static bool loadCompositeShader(S32 shader_level);
    static bool loadAlphaShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadPBRAlphaShaders(S32 shader_level, bool use_sun_shadow);
    static bool loadFullbrightAlphaShaders(S32 shader_level);
    static bool loadMaterialAlphaShaders(S32 shader_level, bool use_sun_shadow,
                                         std::vector<LLGLSLShader*>& shader_list);
    static bool shadersReady();
    static void discardCapture();
    static void setVanillaFallback(bool active);
    static void beginCapture();
    static void endCapture();
    static void releaseResources(bool preserve_node_pool);
    struct Resources
    {
        GLuint heads = 0;
        GLuint counts = 0;
        GLuint headFBO = 0;
        GLuint nodes = 0;
        GLuint control = 0;
        U32 capacity = 0;
        U32 peakNodes = 0;
        U32 overflowCount = 0;
        bool available = false;
    };
    static bool captureOverflowed(U32 required_nodes, U32 overflow_flag);
    static void recordCaptureStats(U32 nodes, U32 maximum_list, bool mouselook);
    static void bindCompositeResources();
    static void copyOpaqueScene(LLRenderTarget& screen);
    static void prepareResourceAllocation();
    static bool beginResourceAllocation(U32 width, U32 height);
    static bool allocateCaptureImages(U32 width, U32 height);
    static void allocateNodePool(U32 width, U32 height, bool capture_images_ready);
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
