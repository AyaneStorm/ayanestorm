/**
 * @file asmilkyway.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local real-sky Milky Way and deep-sky glow, drawn on the sky
 * dome from a catalogue-derived equirectangular texture.
 *
 * Textures (app_settings/stars, built by
 * scripts/content_tools/as_build_milky_way.py; sources and credits in
 * app_settings/stars/LICENSE-celestial-data.txt): as_milky_way.jpg (the band,
 * NASA SVS Deep Star Maps 2020 Gaia layer) and as_deep_sky.png (nebulae),
 * equirectangular J2000, gamma-encoded RGB. Rotates with the real-sky stars
 * through
 * ASStars::localToEquatorial().
 */

#include "llviewerprecompiledheaders.h"

#include "asmilkyway.h"

#include "asdofrenderer.h"
#include "asstars.h"
#include "llappviewer.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llimage.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llviewercontrol.h"

namespace
{
    LLGLSLShader sMilkyWayProgram;
    // Local->equatorial rotation as three mat3 columns (no hashed-name
    // uniformMatrix3fv in LLGLSLShader).
    const LLStaticHashedString sRotation0("mw_rot0");
    const LLStaticHashedString sRotation1("mw_rot1");
    const LLStaticHashedString sRotation2("mw_rot2");
    const LLStaticHashedString sMilkyWayIntensity("mw_intensity");
    const LLStaticHashedString sDeepSkyIntensity("mw_dso_intensity");
    const LLStaticHashedString sSaturation("mw_saturation");

    // Overall glow level relative to the textures (1 = texture white). The
    // glow is diffuse, so far below the stock star level (x32).
    constexpr F32 GLOW_SCALE = 0.25f;

    // One glow texture: file in app_settings/stars, private sampler and
    // channel, decoded image (kept CPU side for GL restores), GL name.
    struct GlowTexture
    {
        const char* mFile;
        LLStaticHashedString mSampler;
        S32 mChannel = -1;
        GLuint mTexture = 0;
        LLPointer<LLImageRaw> mImage;
        bool mLoadTried = false;

        GlowTexture(const char* file, const char* sampler) : mFile(file), mSampler(sampler) {}

        void release()
        {
            if (mTexture)
            {
                glDeleteTextures(1, &mTexture);
                mTexture = 0;
            }
        }

        // Decodes once, by file extension (JPEG band, PNG deep-sky).
        bool load()
        {
            if (mLoadTried)
            {
                return mImage.notNull();
            }
            mLoadTried = true;
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "stars", mFile);
            LLPointer<LLImageFormatted> image = LLImageFormatted::createFromExtension(path);
            LLPointer<LLImageRaw> raw = new LLImageRaw();
            if (image.isNull() || !image->load(path) || !image->decode(raw, 0.f) || raw->getComponents() != 3)
            {
                LL_WARNS("ASMilkyWay") << "Glow texture missing or invalid: " << path << LL_ENDL;
                return false;
            }
            mImage = raw;
            return true;
        }

        // Uploads on first use, then binds. Unpack state saved and restored
        // as in ascolorlut.cpp.
        bool bind()
        {
            if (mChannel < 0 || !load())
            {
                return false;
            }
            LLTexUnit* unit = gGL.getTexUnit(mChannel);
            if (mTexture)
            {
                unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture);
                return true;
            }

            glGenTextures(1, &mTexture);
            unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture);
            // No mips: they would show a seam where atan() wraps at RA 12h.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            const GLenum stores[] = { GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS,
                                      GL_UNPACK_SKIP_ROWS, GL_UNPACK_SWAP_BYTES };
            GLint saved[5], unpack_buffer;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            for (S32 i = 0; i < 5; ++i)
            {
                glGetIntegerv(stores[i], &saved[i]);
                glPixelStorei(stores[i], i == 0 ? 1 : 0);
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, mImage->getWidth(), mImage->getHeight(), 0,
                         GL_RGB, GL_UNSIGNED_BYTE, mImage->getData());
            for (S32 i = 0; i < 5; ++i)
            {
                glPixelStorei(stores[i], saved[i]);
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
            return true;
        }
    };

    GlowTexture sBand("as_milky_way.jpg", "as_milky_way_map");
    GlowTexture sDeepSky("as_deep_sky.png", "as_deep_sky_map");
    GlowTexture* const sTextures[] = { &sBand, &sDeepSky };
}

void ASMilkyWay::registerShader(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sMilkyWayProgram);
}

bool ASMilkyWay::createShader(S32 shader_level)
{
    sMilkyWayProgram.mName = "AyaneStorm Milky Way Shader";
    sMilkyWayProgram.mShaderFiles.clear();
    sMilkyWayProgram.clearPermutations();
    sMilkyWayProgram.mFeatures.isDeferred = true;
    sMilkyWayProgram.mShaderFiles.emplace_back("deferred/asmilkywayV.glsl", GL_VERTEX_SHADER);
    sMilkyWayProgram.mShaderFiles.emplace_back("deferred/asmilkywayF.glsl", GL_FRAGMENT_SHADER);
    sMilkyWayProgram.mShaderLevel = shader_level;
    sMilkyWayProgram.mShaderGroup = LLGLSLShader::SG_SKY;
    // Match the deferred sky render-target layout (see ASAurora::createShader).
    if (gSavedSettings.getBOOL("RenderEnableEmissiveBuffer"))
    {
        sMilkyWayProgram.addPermutation("HAS_EMISSIVE", "1");
    }
    if (!sMilkyWayProgram.createShader())
    {
        return false;
    }

    // Private samplers on the first free channels (the viewer only assigns
    // channels to its reserved sampler names).
    for (GlowTexture* texture : sTextures)
    {
        const S32 channel = sMilkyWayProgram.mActiveTextureChannels;
        texture->mChannel = -1;
        if (sMilkyWayProgram.getUniformLocation(texture->mSampler) < 0 ||
            channel >= gGLManager.mNumTextureImageUnits || channel >= S32(LL_NUM_TEXTURE_LAYERS))
        {
            continue;
        }
        texture->mChannel = channel;
        sMilkyWayProgram.bind();
        sMilkyWayProgram.uniform1i(texture->mSampler, channel);
        sMilkyWayProgram.unbind();
        sMilkyWayProgram.mActiveTextureChannels += 1;
    }
    return true;
}

void ASMilkyWay::unloadShader()
{
    sMilkyWayProgram.unload();
    for (GlowTexture* texture : sTextures)
    {
        texture->release();
        texture->mChannel = -1;
    }
}

bool ASMilkyWay::configureShader(bool hdri_sky)
{
    static LLCachedControl<bool> stars_enabled(gSavedSettings, "ASStarsEnabled", true);
    static LLCachedControl<bool> enabled(gSavedSettings, "ASMilkyWayEnabled", true);
    static LLCachedControl<F32> intensity(gSavedSettings, "ASMilkyWayIntensity", 1.f);
    static LLCachedControl<F32> dso_intensity(gSavedSettings, "ASMilkyWayDeepSkyIntensity", 1.f);
    static LLCachedControl<F32> saturation(gSavedSettings, "ASMilkyWaySaturation", 1.f);

    const LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (hdri_sky || !sky || !stars_enabled || !enabled || !ASStars::realSkyActive() ||
        !sMilkyWayProgram.isComplete())
    {
        return false;
    }

    // Night factor exactly as the stars: EEP star brightness through the
    // same smoothstep(0, 0.9) as starsF.glsl, so the glow comes and goes
    // with the stars.
    const F32 star_alpha = llclamp(sky->getStarBrightness() / 500.f / 0.9f, 0.f, 1.f);
    const F32 night = star_alpha * star_alpha * (3.f - 2.f * star_alpha);
    const F32 level = night * GLOW_SCALE;
    if (level <= 0.0001f || (intensity <= 0.f && dso_intensity <= 0.f))
    {
        return false;
    }

    sMilkyWayProgram.bind();
    // Both samplers must be bound: an unbound one would read whatever its
    // unit holds.
    if (!sBand.bind() || !sDeepSky.bind())
    {
        sMilkyWayProgram.unbind();
        return false;
    }
    F32 rotation[9];
    ASStars::localToEquatorial(ASDoFRenderer::starRotationTime(gFrameTimeSeconds), rotation);
    sMilkyWayProgram.uniform3fv(sRotation0, 1, rotation);
    sMilkyWayProgram.uniform3fv(sRotation1, 1, rotation + 3);
    sMilkyWayProgram.uniform3fv(sRotation2, 1, rotation + 6);
    sMilkyWayProgram.uniform1f(sMilkyWayIntensity, llmax((F32)intensity, 0.f) * level);
    sMilkyWayProgram.uniform1f(sDeepSkyIntensity, llmax((F32)dso_intensity, 0.f) * level);
    sMilkyWayProgram.uniform1f(sSaturation, llmax((F32)saturation, 0.f));
    return true;
}

LLGLSLShader& ASMilkyWay::getShader()
{
    return sMilkyWayProgram;
}
