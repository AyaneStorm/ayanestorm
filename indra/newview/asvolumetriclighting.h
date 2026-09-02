/**
 * @file asvolumetriclighting.h
 * @brief AyaneStorm optional volumetric (god-ray) lighting pass.
 * @author chanayane@firestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
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
 * $/LicenseInfo$
 */

#ifndef AS_VOLUMETRICLIGHTING_H
#define AS_VOLUMETRICLIGHTING_H

#include "llrendertarget.h"

class LLPipeline;
class LLGLSLShader;

// Raymarched sun/moon god-ray pass. Reuses the existing cascaded shadow maps
// and G-buffer, so it costs nothing beyond an extra half-res raymarch and
// composite when disabled or unsupported.
//
// Firestorm/LL files only ever call the statics below; all state and shader
// objects live here so the feature can be extracted cleanly, matching the
// shape of FSExactOIT/FSAVBOIT (see fsexactoit.h).
class ASVolumetricLighting
{
public:
    // Registers reset buttons used by the standalone settings panel.
    static void registerUICallbacks();

    // Cache-salt string folded into the deferred shader cache hash
    // (llviewershadermgr.cpp) so edited .glsl files are not served stale.
    static const char* shaderCacheRevision();

    // GL/GLSL floor check only (no settings read). GLSL 4.00 is the floor,
    // deliberately lower than FSAVBOIT's GLSL 4.30 floor: this feature is
    // designed to also run on macOS, which caps at OpenGL 4.1 / GLSL 4.10.
    static bool isSupported();

    // User setting AND hardware support AND shadows actually being rendered
    // (the pass has nothing to sample without RenderShadowDetail > 0).
    static bool isEnabled();

    static bool loadShaders(S32 shader_level);
    static void unloadShaders();

    static void allocateResources(U32 width, U32 height);
    static void releaseResources();

    // Raymarches sun/moon illumination into a half-res target, then
    // composites the upsampled result into `screen` as
    // scene * transmittance + scatter (attenuating the existing scene by
    // Beer-Lambert transmittance, not just adding scatter on top of it). Call
    // from LLPipeline::renderDeferredLighting() after transparency/OIT and
    // after restoring any required rigged-alpha depth. The caller must flush
    // an already-bound `screen` first; render targets cannot nest themselves.
    // No-ops internally when !isEnabled() or during cube snapshots.
    static void renderPass(LLPipeline& pipeline, LLRenderTarget& screen);

    // Bind the cumulative depth atlas used by transparent material shaders.
    static void bindTransparencyAtlas(LLGLSLShader& shader);

    static S32 getSampleCount();
    static S32 getEdgeSampleMultiplier();
    static F32 getScatterAlbedo();
    static F32 getScatterAsymmetry(bool sun_up);
    static F32 getScatterDensity();

    // Projected visible moon fraction shared by god rays and the circular halo.
    static F32 getMoonPhaseIlluminatedFraction();

    // Debug: when RenderVolumetricLightingDebug != 0, renderPass() bypasses
    // the additive blend and opaquely overwrites `screen` with the raw
    // half-res raymarch output (upsampled, no tonemap), so the raw scatter
    // signal can be inspected directly instead of through the composited,
    // tonemapped result. Temporary - remove once the effect is confirmed
    // working end-to-end.
    static S32 getDebugMode();

private:
    static void applyDirectionalInvariants(LLGLSLShader& shader, LLPipeline& pipeline,
                                           bool sun_source);
    static void renderLocalLights(LLPipeline& pipeline);
    static bool renderTransparencyAtlas(LLPipeline& pipeline, F32 attenuate_scene_strength);
    static void releaseAtlasIntegralAttachments();

    static bool sSupportChecked;
    static bool sSupported;
    static bool sShadersLoaded;

    static LLRenderTarget sVolumetricTarget;
    static LLRenderTarget sTransparencyAtlas;

    // The atlas is built one tile (slice) per draw call instead of all 16 in
    // a single full-resolution draw, so each slice only computes its own new
    // depth segment instead of redundantly re-summing every earlier segment
    // (see asVolumetricAtlasF.glsl's file header for the full rationale).
    // Every slice's draw therefore needs the PREVIOUS slice's raw cumulative
    // integral as an input while writing the CURRENT slice's integral as an
    // output - since a texture cannot be sampled while simultaneously bound
    // as a framebuffer attachment, this needs two separate raw textures that
    // swap roles (source/destination) each slice, attached as a second color
    // attachment alongside sTransparencyAtlas's own real-color attachment 0
    // via direct glFramebufferTexture2D calls (LLRenderTarget's managed
    // attachment APIs are mutually exclusive with each other - neither
    // supports "swap one attachment's texture on an existing multi-
    // attachment target" - see renderTransparencyAtlas()'s definition).
    static U32 sAtlasIntegralTex[2];
    static U32 sAtlasFBO;
    static U32 sAtlasIntegralWidth;
    static U32 sAtlasIntegralHeight;

    // Consumer bindings happen after atlas production in the frame. Keep a
    // conservative demand history: two consecutive frames with no consumer
    // are required before production is skipped. A consumer that returns on
    // a skipped frame gets the shader's no-atlas fallback for that frame and
    // re-enables production for the next one, never sampling stale contents.
    static bool sAtlasConsumerSeen;
    static bool sAtlasProducedThisFrame;
    static U32 sAtlasUnusedFrames;
};

#endif // AS_VOLUMETRICLIGHTING_H
