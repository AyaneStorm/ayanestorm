/**
 * @file fsavboit.h
 * @brief Approximate adaptive voxel-based order-independent transparency.
 * @author chanayane@firestorm
 */

#ifndef FS_AVBOIT_H
#define FS_AVBOIT_H

#include "llgl.h"
#include "llmaterial.h"

#include <vector>

class LLGLSLShader;
class LLRenderTarget;
class LLSD;
class LLDrawPoolAlpha;
class LLDrawInfo;
class LLPipeline;

class FSAVBOIT
{
public:
    static const char* shaderCacheRevision();
    static bool requested();
    static void loadShaders(S32 shader_level);
    static void registerShaders(std::vector<LLGLSLShader*>& shader_list);
    static void unloadShaders();
    static bool shadersReady();
    using PrepareShader = void (*)(LLGLSLShader*, bool, F32);
    static void beginFrame();
    static bool captureActive();
    static bool captureCompleted();
    static bool renderPostDeferredCapture(
        LLDrawPoolAlpha& pool, PrepareShader prepare, F32 water_sign,
        LLGLSLShader*& emissive_shader, LLGLSLShader*& pbr_emissive_shader);
    static bool configureCapturedDrawIfActive(LLGLSLShader* shader);
    static bool handleCapturedEmissives(
        LLDrawPoolAlpha& pool, bool depth_only,
        std::vector<LLDrawInfo*>& emissives,
        std::vector<LLDrawInfo*>& pbr_emissives,
        std::vector<LLDrawInfo*>& rigged_emissives,
        std::vector<LLDrawInfo*>& pbr_rigged_emissives);
    static void configureGLTFCapturedDraw(LLGLSLShader& shader);
    static bool finishFrame(LLPipeline& pipeline, LLRenderTarget& screen);
    static LLGLSLShader& gltfProgram(LLGLSLShader& ordinary_program);
    static LLGLSLShader* alphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* pbrAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* fullbrightAlphaShader(LLGLSLShader* ordinary);
    static LLGLSLShader* materialAlphaShader(U32 mask, LLGLSLShader* ordinary);
    static LLGLSLShader* emissiveShader();
    static LLGLSLShader* pbrGlowShader();
    static void allocateResources(U32 width, U32 height);
    static void releaseResources();
    static void appendDiagnostics(LLSD& info);
    static bool beginDirectFrame(LLRenderTarget& screen);
    static void beginDirectRasterPass(S32 pass);
    static void configureDirectRasterShader(LLGLSLShader* shader);
    static void rasterizeConservativeBounds();
    static void finishDirectOccupancy();
    // A9: the pass-1 (extinction raster) setup that used to be
    // finishDirectOccupancy()'s last two lines, split out so
    // renderPostDeferredCapture() can insert pass 3 (front key) between
    // finishDirectOccupancy()'s compute/cell-depth-bake work and pass 1's
    // own target bind, while gAVBOITOpaqueTarget is the current target.
    static void beginPass1();
    static void finishDirectExtinction();
    static void finishDirectColorRaster();
    static bool finishDirectFrame(LLRenderTarget& screen);
    static bool directFrameReady();

private:
    static bool supported();
    static bool available();
    // Chooses the virtual depth-slice domain for this session, falling back to
    // the presentation's baseline when the driver or video memory cannot support
    // the high-resolution path.
    static void selectVirtualDomain();
    static bool allocateVolume(U32 width, U32 height);

    struct Resources
    {
        GLuint extinction = 0;
        GLuint transmittance = 0;
        GLuint zeroTransmittanceDepth = 0;
        GLuint extinctionOverflowDepth = 0;
        GLuint occupancy = 0;
        GLuint warp = 0;
        // Transient prefix-scan array. Shared memory cannot hold the
        // high-resolution virtual domain, so the scan runs in shader storage.
        GLuint warpScan = 0;
        GLuint tileOccupancy = 0;
        GLuint work = 0;
        GLuint diagnostics = 0;
        GLuint accumulatedColorGlow = 0;
        GLuint accumulatedWeight = 0;
        GLuint accumulatedExtinction = 0;
        // A9: per-pixel front key (nearest, second-nearest distinct depth),
        // full resolution. See FSAVBOIT::renderPostDeferredCapture()'s pass 3.
        GLuint frontKey0 = 0;
        GLuint frontKey1 = 0;
        // Third- and fourth-nearest distinct depth. See doc/ayanestorm-oit-
        // avboit-glass-darkening.md: keeps hair's two exact strands intact
        // behind a pane (frontKey2) or a thick two-face pane (frontKey3),
        // either of which would otherwise consume a key slot itself.
        GLuint frontKey2 = 0;
        GLuint frontKey3 = 0;
        // FBO wrapping frontKey0/1/2/3 as color attachments, used only by
        // the glClearTexImage fallback path (drivers below GL 4.4).
        GLuint frontKeyFBO = 0;
        U32 volumeWidth = 0;
        U32 volumeHeight = 0;
        U32 viewportWidth = 0;
        U32 viewportHeight = 0;
        bool available = false;
    };
    static Resources sResources;
    static S32 sDirectRasterPass;
    static bool sDirectFrameReady;
    static bool sCaptureActive;
    static bool sCaptureCompleted;
};

#endif
