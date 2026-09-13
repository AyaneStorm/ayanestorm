/**
 * @file ascolorgrading.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local photographic color grading and preset storage.
 */
#include "llviewerprecompiledheaders.h"

#include "ascolorgrading.h"
#include "ascolorlut.h"

#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsdserialize.h"
#include "llshadermgr.h"
#include "lluri.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "v4color.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>

extern bool gCubeSnapshot;
extern bool gSnapshot;
extern bool gSnapshotNoPost;
extern U32 gFrameCount;

namespace
{
    LLGLSLShader sFinalProgram;
    bool sPreviewBypass = false;
    U32 sStaticGrainSeed = 0;

    const char* const BAND_NAMES[ASColorGrading::BAND_COUNT] =
        { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta",
          "Gray1", "Gray2", "Gray3", "Gray4", "Gray5", "Gray6", "Gray7", "Gray8",
          "RedSkin2", "RedSkin4", "RedSkin6", "RedSkin8", "Skin2", "Skin4", "Skin6", "Skin8" };
    const char* const COMPONENTS[8] =
        { "Hue", "Saturation", "Lightness", "Strength", "HueRange",
          "ChromaRange", "LightnessRange", "Softness" };
    const char* const BUILTIN_PRESETS[] =
        { "[AS] Neutral", "[AS] Sepia", "[AS] Cyanotype", "[AS] Selenium", "[AS] Black & White",
          "[AS] Warm Vintage", "[AS] Cool Cinematic", "[AS] Bleach Bypass", "[AS] Vivid" };
    constexpr S32 CURRENT_PRESET_VERSION = 2;

    const LLStaticHashedString sLinearEnabled("as_color_grade_linear_enabled");
    const LLStaticHashedString sExposure("as_color_grade_exposure");
    const LLStaticHashedString sWhiteBalance("as_color_grade_white_balance");
    const LLStaticHashedString sBasic1("as_color_grade_basic1");
    const LLStaticHashedString sBasic2("as_color_grade_basic2");
    const LLStaticHashedString sBasic3("as_color_grade_basic3");
    const LLStaticHashedString sBands("as_color_grade_bands");
    const LLStaticHashedString sBandSelectionColors("as_color_grade_band_selection_colors");
    const LLStaticHashedString sBandParameters("as_color_grade_band_parameters");
    const LLStaticHashedString sBandRanges("as_color_grade_band_ranges");
    const LLStaticHashedString sColorize("as_color_grade_colorize");
    const LLStaticHashedString sSplitToning1("as_color_grade_split_toning1");
    const LLStaticHashedString sSplitToning2("as_color_grade_split_toning2");
    const LLStaticHashedString sNegative("as_color_grade_negative");
    const LLStaticHashedString sGrain("as_color_grade_grain");
    const LLStaticHashedString sGrainSeed("as_color_grade_grain_seed");
    const LLStaticHashedString sSnapshotTile("as_color_grade_snapshot_tile");

    F32 normalized(const char* name)
    {
        return llclamp(gSavedSettings.getF32(name) * 0.01f, -1.f, 1.f);
    }

    // Convert persisted sRGB selection colors once per presentation, not per pixel.
    void srgbToOklab(const LLColor4& color, F32* output)
    {
        F32 rgb[3];
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 value = llclamp(color.mV[i], 0.f, 1.f);
            rgb[i] = value <= 0.04045f ? value / 12.92f :
                std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        const F32 l = std::cbrt(0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]);
        const F32 m = std::cbrt(0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]);
        const F32 s = std::cbrt(0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]);
        output[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
        output[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
        output[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    }

    // Remove discarded mixer values from the next saved user-settings file.
    void retireObsoleteMixerSettings()
    {
        const char* const bands[] =
            { "Skin1", "Skin3", "Skin5", "Skin7", "RedSkin1", "RedSkin3", "RedSkin5", "RedSkin7" };
        const char* const components[] =
            { "Hue", "Saturation", "Luminance", "Lightness", "Strength",
              "Tolerance", "HueRange", "ChromaRange", "ShadeRange", "LightnessRange",
              "Softness", "TargetLightness", "TargetColor", "SelectionColor" };
        for (const char* band : bands)
            for (const char* component : components)
                if (LLControlVariable* control = gSavedSettings.getControl(
                    "ASColorGrade" + std::string(band) + component))
                    control->setPersist(LLControlVariable::PERSIST_NO);
        // Retire names superseded by the current selection and adjustment terminology.
        const char* const obsolete_components[] =
            { "TargetLightness", "ShadeRange", "Luminance", "Tolerance", "TargetColor" };
        for (S32 band = 0; band < ASColorGrading::BAND_COUNT; ++band)
            for (const char* component : obsolete_components)
                if (LLControlVariable* control = gSavedSettings.getControl(
                    "ASColorGrade" + std::string(BAND_NAMES[band]) + component))
                    control->setPersist(LLControlVariable::PERSIST_NO);
        if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeColorizeLuminance"))
            control->setPersist(LLControlVariable::PERSIST_NO);
    }

    std::string presetDir()
    {
        const std::string root = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "presets");
        LLFile::mkdir(root);
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "presets", "color_grading");
        LLFile::mkdir(dir);
        return dir;
    }

    bool validPresetName(std::string name)
    {
        LLStringUtil::trim(name);
        return !name.empty() && !ASColorGrading::isReadOnlyPreset(name) && name != "Custom" &&
               name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    void applyBuiltinPreset(const std::string& name)
    {
        ASColorGrading::resetAll();
        const std::string preset = name.compare(0, 5, "[AS] ") == 0 ? name.substr(5) : name;
        std::map<std::string, F32> values;
        if (preset == "Sepia")
            values = { {"ASColorGradeColorizeHue",35.f}, {"ASColorGradeColorizeSaturation",25.f},
                {"ASColorGradeColorizeLightness",0.f}, {"ASColorGradeExposure",.1f},
                {"ASColorGradeBrightness",4.f}, {"ASColorGradeContrast",12.f}, {"ASColorGradeHighlights",-15.f},
                {"ASColorGradeShadows",12.f}, {"ASColorGradeBlacks",-8.f}, {"ASColorGradeGrainAmount",18.f}, {"ASColorGradeGrainSize",45.f},
                {"ASColorGradeGrainRoughness",65.f}, {"ASColorGradeGrainColor",0.f} };
        else if (preset == "Cyanotype")
            values = { {"ASColorGradeColorizeHue",215.f}, {"ASColorGradeColorizeSaturation",25.f},
                {"ASColorGradeColorizeLightness",0.f}, {"ASColorGradeContrast",18.f},
                {"ASColorGradeHighlights",-10.f}, {"ASColorGradeShadows",-8.f}, {"ASColorGradeBlacks",-12.f},
                {"ASColorGradeGrainAmount",12.f}, {"ASColorGradeGrainSize",38.f},
                {"ASColorGradeGrainRoughness",58.f}, {"ASColorGradeGrainColor",0.f} };
        else if (preset == "Selenium")
            values = { {"ASColorGradeContrast",21.f}, {"ASColorGradeHighlights",12.f},
                {"ASColorGradeShadows",11.f}, {"ASColorGradeWhites",9.f}, {"ASColorGradeBlacks",2.f},
                {"ASColorGradeTemperature",54.f}, {"ASColorGradeTint",8.f}, {"ASColorGradeVibrance",12.f},
                {"ASColorGradeColorizeHue",218.f}, {"ASColorGradeColorizeSaturation",10.f},
                {"ASColorGradeColorizeLightness",0.f}, {"ASColorGradeGrainAmount",52.f},
                {"ASColorGradeGrainSize",18.f}, {"ASColorGradeGrainRoughness",56.f},
                {"ASColorGradeGrainColor",10.f} };
        else if (preset == "Black & White")
            values = { {"ASColorGradeSaturation",-100.f}, {"ASColorGradeContrast",12.f}, {"ASColorGradeShadows",8.f},
                {"ASColorGradeBlacks",-10.f}, {"ASColorGradeGrainAmount",10.f}, {"ASColorGradeGrainColor",0.f} };
        else if (preset == "Warm Vintage")
            values = { {"ASColorGradeTemperature",55.f}, {"ASColorGradeTint",8.f}, {"ASColorGradeContrast",-8.f},
                {"ASColorGradeHighlights",-25.f}, {"ASColorGradeShadows",18.f}, {"ASColorGradeBlacks",8.f},
                {"ASColorGradeSaturation",-18.f}, {"ASColorGradeVibrance",12.f}, {"ASColorGradeGrainAmount",20.f},
                {"ASColorGradeGrainSize",55.f}, {"ASColorGradeGrainRoughness",70.f}, {"ASColorGradeGrainColor",10.f} };
        else if (preset == "Cool Cinematic")
            values = { {"ASColorGradeTemperature",-35.f}, {"ASColorGradeTint",-8.f}, {"ASColorGradeContrast",18.f},
                {"ASColorGradeHighlights",-18.f}, {"ASColorGradeShadows",-12.f}, {"ASColorGradeBlacks",-12.f},
                {"ASColorGradeSaturation",-8.f}, {"ASColorGradeVibrance",20.f}, {"ASColorGradeOrangeSaturation",10.f},
                {"ASColorGradeOrangeLightness",5.f}, {"ASColorGradeAquaSaturation",15.f},
                {"ASColorGradeBlueSaturation",20.f}, {"ASColorGradeBlueLightness",-8.f} };
        else if (preset == "Bleach Bypass")
            values = { {"ASColorGradeSaturation",-65.f}, {"ASColorGradeContrast",35.f}, {"ASColorGradeHighlights",-10.f},
                {"ASColorGradeShadows",-20.f}, {"ASColorGradeBlacks",-25.f}, {"ASColorGradeGrainAmount",16.f},
                {"ASColorGradeGrainRoughness",60.f}, {"ASColorGradeGrainColor",0.f} };
        else if (preset == "Vivid")
            values = { {"ASColorGradeContrast",10.f}, {"ASColorGradeSaturation",8.f},
                {"ASColorGradeVibrance",35.f}, {"ASColorGradeHighlights",-8.f}, {"ASColorGradeShadows",8.f} };
        for (const auto& value : values) gSavedSettings.setF32(value.first, value.second);
        if (preset == "Sepia" || preset == "Cyanotype" || preset == "Selenium")
            gSavedSettings.setBOOL("ASColorGradeColorizeEnabled", true);
    }

    bool validatedPresetValue(const std::string& name, const LLSD& input, F32& output)
    {
        if (!input.isReal() && !input.isInteger()) return false;
        output = (F32)input.asReal();
        if (!std::isfinite(output)) return false;
        F32 minimum = -100.f;
        F32 maximum = 100.f;
        if (name == "ASColorGradeExposure") { minimum = -5.f; maximum = 5.f; }
        else if (name == "ASColorGradeHue") { minimum = -180.f; maximum = 180.f; }
        else if (name == "ASColorGradeColorizeHue") { minimum = 0.f; maximum = 360.f; }
        else if (name == "ASColorGradeColorizeSaturation") { minimum = 0.f; maximum = 100.f; }
        else if (name == "ASColorGradeSplitHighlightsHue" || name == "ASColorGradeSplitShadowsHue")
            { minimum = 0.f; maximum = 360.f; }
        else if (name.size() >= 3 && name.compare(name.size() - 3, 3, "Hue") == 0)
            { minimum = -180.f; maximum = 180.f; }
        else if (name == "ASColorGradeSplitHighlightsSaturation" || name == "ASColorGradeSplitShadowsSaturation")
            { minimum = 0.f; maximum = 100.f; }
        else if (name == "ASColorGradeTemperature") { minimum = -200.f; maximum = 200.f; }
        else if (name == "ASColorGradeTint") { minimum = -200.f; maximum = 200.f; }
        else if (name == "ASColorGradeLUTStrength") { minimum = 0.f; maximum = 100.f; }
        else if (name.find("ASColorGradeGrain") == 0) { minimum = 0.f; maximum = 100.f; }
        else if (name.find("Strength") != std::string::npos ||
                 name.find("Range") != std::string::npos ||
                 name.find("Softness") != std::string::npos)
            { minimum = 0.f; maximum = 100.f; }
        output = llclamp(output, minimum, maximum);
        return true;
    }

    bool validatedPresetColor(const LLSD& input, LLColor4& output)
    {
        if (!input.isArray() || input.size() != 4) return false;
        for (S32 i = 0; i < 4; ++i)
        {
            if (!input[i].isReal() && !input[i].isInteger()) return false;
            const F32 value = (F32)input[i].asReal();
            if (!std::isfinite(value) || value < 0.f || value > 1.f) return false;
            output.mV[i] = value;
        }
        output.mV[VALPHA] = 1.f;
        return true;
    }

    // Version 1 used earlier mixer terminology. Migrate only the imported LLSD;
    // runtime settings and newly saved presets use the current names exclusively.
    void migratePresetValues(S32 version, LLSD& values)
    {
        if (version != 1) return;
        const auto migrate = [&values](const std::string& old_name, const std::string& new_name)
        {
            if (!values.has(new_name) && values.has(old_name)) values[new_name] = values[old_name];
        };
        migrate("ASColorGradeColorizeLuminance", "ASColorGradeColorizeLightness");
        for (S32 band = 0; band < ASColorGrading::BAND_COUNT; ++band)
        {
            const std::string prefix = "ASColorGrade" + std::string(BAND_NAMES[band]);
            migrate(prefix + "Luminance", prefix + "Lightness");
            migrate(prefix + "Tolerance", prefix + "HueRange");
            migrate(prefix + "ShadeRange", prefix + "LightnessRange");
            migrate(prefix + "TargetColor", prefix + "SelectionColor");
        }
    }

    // Bradford adaptation from D65 to a bounded creative cool/warm and tint white point.
    void whiteBalanceMatrix(F32 out[9])
    {
        // Preserve the original response through +/-100 and extend it to
        // +/-200 for stronger creative cooling and warming.
        const F32 temperature = llclamp(gSavedSettings.getF32("ASColorGradeTemperature") * 0.01f, -2.f, 2.f);
        // Match the extended temperature range while preserving the original
        // response through +/-100.
        const F32 tint = llclamp(gSavedSettings.getF32("ASColorGradeTint") * 0.01f, -2.f, 2.f);
        if (temperature == 0.f && tint == 0.f)
        {
            const F32 identity[9] = { 1,0,0, 0,1,0, 0,0,1 };
            memcpy(out, identity, sizeof(identity));
            return;
        }

        // Bounded xy offsets are intentionally creative rather than Kelvin-labelled.
        const F32 src_xyz[3] = { 0.95047f, 1.f, 1.08883f };
        F32 x = llclamp(0.31271f + temperature * 0.045f + tint * 0.008f, 0.22f, 0.42f);
        F32 y = llclamp(0.32902f + temperature * 0.018f - tint * 0.035f, 0.22f, 0.42f);
        F32 dst_xyz[3] = { x / y, 1.f, (1.f - x - y) / y };
        const F32 b[9] = { .8951f,.2664f,-.1614f, -.7502f,1.7135f,.0367f, .0389f,-.0685f,1.0296f };
        const F32 bi[9] = { .986993f,-.147054f,.159963f, .432305f,.51836f,.049291f, -.008529f,.040043f,.968487f };
        F32 src_lms[3] = {}, dst_lms[3] = {};
        for (S32 r = 0; r < 3; ++r)
        {
            for (S32 c = 0; c < 3; ++c)
            {
                src_lms[r] += b[r * 3 + c] * src_xyz[c];
                dst_lms[r] += b[r * 3 + c] * dst_xyz[c];
            }
        }
        F32 d[3];
        for (S32 i = 0; i < 3; ++i) d[i] = llclamp(dst_lms[i] / src_lms[i], 0.5f, 2.f);
        F32 xyz_adapt[9] = {};
        for (S32 r = 0; r < 3; ++r)
            for (S32 c = 0; c < 3; ++c)
                for (S32 k = 0; k < 3; ++k)
                    xyz_adapt[r * 3 + c] += bi[r * 3 + k] * d[k] * b[k * 3 + c];

        const F32 rgb_to_xyz[9] = { .4124564f,.3575761f,.1804375f, .2126729f,.7151522f,.0721750f, .0193339f,.1191920f,.9503041f };
        const F32 xyz_to_rgb[9] = { 3.2404542f,-1.5371385f,-.4985314f, -.9692660f,1.8760108f,.0415560f, .0556434f,-.2040259f,1.0572252f };
        F32 temp[9] = {};
        for (S32 r = 0; r < 3; ++r)
            for (S32 c = 0; c < 3; ++c)
                for (S32 k = 0; k < 3; ++k)
                    temp[r * 3 + c] += xyz_adapt[r * 3 + k] * rgb_to_xyz[k * 3 + c];
        for (S32 r = 0; r < 3; ++r)
            for (S32 c = 0; c < 3; ++c)
            {
                out[r * 3 + c] = 0.f;
                for (S32 k = 0; k < 3; ++k) out[r * 3 + c] += xyz_to_rgb[r * 3 + k] * temp[k * 3 + c];
            }
    }
}

const std::vector<std::string>& ASColorGrading::settingNames()
{
    static std::vector<std::string> names;
    if (names.empty())
    {
        const char* basic[] = { "ASColorGradeExposure", "ASColorGradeBrightness", "ASColorGradeContrast",
            "ASColorGradeTemperature", "ASColorGradeTint", "ASColorGradeShadows", "ASColorGradeHighlights",
            "ASColorGradeBlacks", "ASColorGradeWhites", "ASColorGradeSaturation", "ASColorGradeVibrance",
            "ASColorGradeHue", "ASColorGradeLUTStrength", "ASColorGradeGrainAmount", "ASColorGradeGrainSize",
            "ASColorGradeGrainRoughness", "ASColorGradeGrainColor", "ASColorGradeColorizeHue",
            "ASColorGradeColorizeSaturation", "ASColorGradeColorizeLightness",
            "ASColorGradeSplitHighlightsHue", "ASColorGradeSplitHighlightsSaturation",
            "ASColorGradeSplitBalance", "ASColorGradeSplitShadowsHue", "ASColorGradeSplitShadowsSaturation" };
        names.assign(std::begin(basic), std::end(basic));
        for (S32 band = 0; band < BAND_COUNT; ++band)
            for (const char* component : COMPONENTS)
                names.push_back(bandSettingName((Band)band, component));
    }
    return names;
}

std::string ASColorGrading::bandSettingName(Band band, const std::string& component)
{
    return "ASColorGrade" + std::string(BAND_NAMES[band]) + component;
}

std::string ASColorGrading::selectionColorSettingName(Band band)
{
    return bandSettingName(band, "SelectionColor");
}

void ASColorGrading::registerShaders(std::vector<LLGLSLShader*>& shaders) { shaders.push_back(&sFinalProgram); }

void ASColorGrading::appendLinearShader(LLGLSLShader& shader)
{
    shader.mShaderFiles.emplace_back("deferred/ascolorgradingLinearF.glsl", GL_FRAGMENT_SHADER);
}

bool ASColorGrading::createShaders(S32 shader_level)
{
    retireObsoleteMixerSettings();
    sFinalProgram.mName = "AyaneStorm Color Grading Presentation Shader";
    sFinalProgram.mShaderFiles.clear();
    sFinalProgram.clearPermutations();
    sFinalProgram.mFeatures.isDeferred = true;
    sFinalProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sFinalProgram.mShaderFiles.emplace_back("deferred/ascolorgradingF.glsl", GL_FRAGMENT_SHADER);
    sFinalProgram.addPermutation("HAS_NOISE", "1");
    sFinalProgram.mShaderLevel = shader_level;
    const bool created = sFinalProgram.createShader();
    if (created && !ASColorLUT::configureShader(sFinalProgram))
    {
        sFinalProgram.unload();
        return false;
    }
    return created;
}

void ASColorGrading::unloadShaders()
{
    ASColorLUT::unload();
    sFinalProgram.unload();
}

bool ASColorGrading::isActive()
{
    return gSavedSettings.getBOOL("ASColorGradingEnabled") && sFinalProgram.isComplete() &&
           !sPreviewBypass && !gCubeSnapshot &&
           !gSnapshotNoPost && LLPipeline::RenderBufferVisualization < 0;
}

void ASColorGrading::setPreviewBypass(bool bypass) { sPreviewBypass = bypass; }
bool ASColorGrading::getPreviewBypass() { return sPreviewBypass; }

void ASColorGrading::bindLinearUniforms(LLGLSLShader& shader, bool bypass)
{
    const bool enabled = isActive() && !bypass;
    shader.uniform1i(sLinearEnabled, enabled ? 1 : 0);
    shader.uniform1f(sExposure, enabled ? llclamp(gSavedSettings.getF32("ASColorGradeExposure"), -5.f, 5.f) : 0.f);
    F32 matrix[9];
    if (enabled) whiteBalanceMatrix(matrix);
    else { const F32 identity[9] = {1,0,0,0,1,0,0,0,1}; memcpy(matrix, identity, sizeof(identity)); }
    const GLint location = shader.getUniformLocation(sWhiteBalance);
    if (location >= 0) glUniformMatrix3fv(location, 1, GL_TRUE, matrix);
}

bool ASColorGrading::present(LLRenderTarget& color, LLRenderTarget& depth, LLVertexBuffer& triangle)
{
    if (!isActive() || !sFinalProgram.isComplete() || color.getWidth() <= 0 || color.getHeight() <= 0) return false;
    LL_PROFILE_GPU_ZONE("AyaneStorm Color Grading");
    LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_ALWAYS);
    LLGLDisable blend(GL_BLEND);
    sFinalProgram.bind();
    sFinalProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &color);
    sFinalProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth, true);
    sFinalProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)color.getWidth(), (F32)color.getHeight());
    sFinalProgram.uniform4f(sBasic1, normalized("ASColorGradeBrightness"), normalized("ASColorGradeContrast"),
        normalized("ASColorGradeHighlights"), normalized("ASColorGradeShadows"));
    sFinalProgram.uniform4f(sBasic2, normalized("ASColorGradeWhites"), normalized("ASColorGradeBlacks"),
        normalized("ASColorGradeSaturation"), normalized("ASColorGradeVibrance"));
    sFinalProgram.uniform1i(sNegative, gSavedSettings.getBOOL("ASColorGradeNegativeEnabled") ? 1 : 0);
    sFinalProgram.uniform1f(sBasic3, gSavedSettings.getF32("ASColorGradeHue") * DEG_TO_RAD);
    F32 bands[BAND_COUNT * 3];
    F32 band_selection_colors[BAND_COUNT * 3];
    F32 band_parameters[BAND_COUNT * 3];
    F32 band_ranges[BAND_COUNT * 2];
    for (S32 band = 0; band < BAND_COUNT; ++band)
    {
        bands[band * 3] = gSavedSettings.getF32(bandSettingName((Band)band, "Hue")) * DEG_TO_RAD;
        bands[band * 3 + 1] = normalized(bandSettingName((Band)band, "Saturation").c_str());
        bands[band * 3 + 2] = normalized(bandSettingName((Band)band, "Lightness").c_str());
        srgbToOklab(gSavedSettings.getColor4(selectionColorSettingName((Band)band)),
                    &band_selection_colors[band * 3]);
        band_parameters[band * 3] = llclamp(gSavedSettings.getF32(
            bandSettingName((Band)band, "Strength")) * .01f, 0.f, 1.f);
        band_parameters[band * 3 + 1] = llclamp(gSavedSettings.getF32(
            bandSettingName((Band)band, "HueRange")) * .01f, 0.f, 1.f);
        band_parameters[band * 3 + 2] = llclamp(gSavedSettings.getF32(
            bandSettingName((Band)band, "Softness")) * .01f, 0.f, 1.f);
        band_ranges[band * 2] = llclamp(gSavedSettings.getF32(
            bandSettingName((Band)band, "ChromaRange")) * .01f, 0.f, 1.f);
        band_ranges[band * 2 + 1] = llclamp(gSavedSettings.getF32(
            bandSettingName((Band)band, "LightnessRange")) * .01f, 0.f, 1.f);
    }
    sFinalProgram.uniform3fv(sBands, BAND_COUNT, bands);
    sFinalProgram.uniform3fv(sBandSelectionColors, BAND_COUNT, band_selection_colors);
    sFinalProgram.uniform3fv(sBandParameters, BAND_COUNT, band_parameters);
    sFinalProgram.uniform2fv(sBandRanges, BAND_COUNT, band_ranges);
    sFinalProgram.uniform4f(sColorize, gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") ? 1.f : 0.f,
        llclamp(gSavedSettings.getF32("ASColorGradeColorizeHue"), 0.f, 360.f) * DEG_TO_RAD,
        llclamp(gSavedSettings.getF32("ASColorGradeColorizeSaturation") * .01f, 0.f, 1.f),
        normalized("ASColorGradeColorizeLightness"));
    sFinalProgram.uniform4f(sSplitToning1,
        llclamp(gSavedSettings.getF32("ASColorGradeSplitHighlightsHue"), 0.f, 360.f) * DEG_TO_RAD,
        llclamp(gSavedSettings.getF32("ASColorGradeSplitHighlightsSaturation") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeSplitShadowsHue"), 0.f, 360.f) * DEG_TO_RAD,
        llclamp(gSavedSettings.getF32("ASColorGradeSplitShadowsSaturation") * .01f, 0.f, 1.f));
    sFinalProgram.uniform2f(sSplitToning2,
        gSavedSettings.getBOOL("ASColorGradeSplitToningEnabled") ? 1.f : 0.f,
        normalized("ASColorGradeSplitBalance"));
    sFinalProgram.uniform4f(sGrain, llclamp(gSavedSettings.getF32("ASColorGradeGrainAmount") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainSize") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainRoughness") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainColor") * .01f, 0.f, 1.f));
    static U32 snapshot_seed = 0;
    static bool was_snapshot = false;
    if (gSnapshot && !was_snapshot)
        snapshot_seed = gSavedSettings.getBOOL("ASColorGradeGrainStatic") ? sStaticGrainSeed : gFrameCount;
    was_snapshot = gSnapshot;
    // Static grain holds one spatial pattern until the user requests another.
    const U32 live_seed = gSavedSettings.getBOOL("ASColorGradeGrainStatic") ? sStaticGrainSeed : gFrameCount;
    sFinalProgram.uniform1f(sGrainSeed, (F32)(gSnapshot ? snapshot_seed : live_seed));
    const F32 zoom = llmax(LLViewerCamera::getInstance()->getZoomFactor(), 1.f);
    const S32 tile = LLViewerCamera::getInstance()->getZoomSubRegion();
    const S32 row = llceil(zoom);
    sFinalProgram.uniform3f(sSnapshotTile, zoom, zoom > 1.f ? (F32)(tile % row) : 0.f, zoom > 1.f ? (F32)(tile / row) : 0.f);
    ASColorLUT::bind(sFinalProgram);
    triangle.setBuffer();
    triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    ASColorLUT::unbind();
    sFinalProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, color.getUsage());
    sFinalProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sFinalProgram.unbind();
    return true;
}

void ASColorGrading::refreshStaticGrain()
{
    ++sStaticGrainSeed;
}

void ASColorGrading::resetAll()
{
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeLUTEnabled")) control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeLUTFile")) control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeNegativeEnabled")) control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeGrainStatic")) control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeColorizeEnabled")) control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeSplitToningEnabled")) control->resetToDefault(true);
    for (const std::string& name : settingNames())
        if (LLControlVariable* control = gSavedSettings.getControl(name)) control->resetToDefault(true);
    for (S32 band = 0; band < BAND_COUNT; ++band)
        if (LLControlVariable* control = gSavedSettings.getControl(selectionColorSettingName((Band)band)))
            control->resetToDefault(true);
}

void ASColorGrading::resetBand(Band band)
{
    for (const char* component : COMPONENTS)
        if (LLControlVariable* control = gSavedSettings.getControl(bandSettingName(band, component)))
            control->resetToDefault(true);
    if (LLControlVariable* control = gSavedSettings.getControl(selectionColorSettingName(band)))
        control->resetToDefault(true);
}

bool ASColorGrading::bandModified(Band band)
{
    for (const char* component : COMPONENTS)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(bandSettingName(band, component)))
            if (control->getValue().asReal() != control->getDefault().asReal()) return true;
    }
    if (LLControlVariable* control = gSavedSettings.getControl(selectionColorSettingName(band)))
        return LLColor4(control->getValue()) != LLColor4(control->getDefault());
    return false;
}

std::vector<std::string> ASColorGrading::listPresets()
{
    std::vector<std::string> result(std::begin(BUILTIN_PRESETS), std::end(BUILTIN_PRESETS));
    LLDirIterator iter(presetDir(), "*.xml");
    std::string file;
    while (iter.next(file))
    {
        if (file.size() > 4)
        {
            const std::string name = LLURI::unescape(file.substr(0, file.size() - 4));
            if (!isReadOnlyPreset(name)) result.push_back(name);
        }
    }
    std::sort(result.begin() + std::size(BUILTIN_PRESETS), result.end());
    return result;
}

bool ASColorGrading::isReadOnlyPreset(const std::string& name)
{
    return std::any_of(std::begin(BUILTIN_PRESETS), std::end(BUILTIN_PRESETS), [&name](const char* builtin)
    {
        return LLStringUtil::compareInsensitive(name, builtin) == 0;
    });
}

bool ASColorGrading::savePreset(const std::string& name)
{
    if (!validPresetName(name)) return false;
    LLSD data; data["version"] = CURRENT_PRESET_VERSION; data["name"] = name;
    data["values"]["ASColorGradeLUTEnabled"] = gSavedSettings.getBOOL("ASColorGradeLUTEnabled");
    data["values"]["ASColorGradeLUTFile"] = gSavedSettings.getString("ASColorGradeLUTFile");
    data["values"]["ASColorGradeNegativeEnabled"] = gSavedSettings.getBOOL("ASColorGradeNegativeEnabled");
    data["values"]["ASColorGradeGrainStatic"] = gSavedSettings.getBOOL("ASColorGradeGrainStatic");
    for (const std::string& setting : settingNames()) data["values"][setting] = gSavedSettings.getLLSD(setting);
    for (S32 band = 0; band < BAND_COUNT; ++band)
    {
        const std::string setting = selectionColorSettingName((Band)band);
        data["values"][setting] = gSavedSettings.getLLSD(setting);
    }
    data["values"]["ASColorGradeColorizeEnabled"] = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled");
    data["values"]["ASColorGradeSplitToningEnabled"] = gSavedSettings.getBOOL("ASColorGradeSplitToningEnabled");
    llofstream file(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml");
    return file.is_open() && LLSDSerialize::toPrettyXML(data, file);
}

bool ASColorGrading::loadPreset(const std::string& name)
{
    for (const char* builtin : BUILTIN_PRESETS)
    {
        if (LLStringUtil::compareInsensitive(name, builtin) == 0)
        {
            applyBuiltinPreset(builtin);
            return true;
        }
    }
    llifstream file(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml");
    LLSD data;
    if (!file.is_open() || LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE ||
        !data.isMap() || !data["version"].isInteger() || !data["values"].isMap()) return false;
    const S32 version = data["version"].asInteger();
    if (version < 1 || version > CURRENT_PRESET_VERSION) return false;
    LLSD values = data["values"];
    migratePresetValues(version, values);
    // Missing LUT fields in older presets restore the disabled default.
    if (values.has("ASColorGradeLUTEnabled") && !values["ASColorGradeLUTEnabled"].isBoolean()) return false;
    const bool lut_enabled = values.has("ASColorGradeLUTEnabled") && values["ASColorGradeLUTEnabled"].asBoolean();
    if (values.has("ASColorGradeLUTFile") && !values["ASColorGradeLUTFile"].isString()) return false;
    const std::string lut_file = values.has("ASColorGradeLUTFile") ? values["ASColorGradeLUTFile"].asString() : "";
    if (!lut_file.empty() && !ASColorLUT::validName(lut_file)) return false;
    // Older presets omit Negative and retain the default non-inverted image.
    if (values.has("ASColorGradeNegativeEnabled") && !values["ASColorGradeNegativeEnabled"].isBoolean()) return false;
    const bool negative = values.has("ASColorGradeNegativeEnabled") && values["ASColorGradeNegativeEnabled"].asBoolean();
    // Older presets omit static grain and retain the animated default.
    if (values.has("ASColorGradeGrainStatic") && !values["ASColorGradeGrainStatic"].isBoolean()) return false;
    const bool grain_static = values.has("ASColorGradeGrainStatic") && values["ASColorGradeGrainStatic"].asBoolean();
    if (values.has("ASColorGradeColorizeEnabled") && !values["ASColorGradeColorizeEnabled"].isBoolean()) return false;
    const bool colorize = values.has("ASColorGradeColorizeEnabled") && values["ASColorGradeColorizeEnabled"].asBoolean();
    if (values.has("ASColorGradeSplitToningEnabled") && !values["ASColorGradeSplitToningEnabled"].isBoolean()) return false;
    const bool split_toning = values.has("ASColorGradeSplitToningEnabled") && values["ASColorGradeSplitToningEnabled"].asBoolean();
    std::map<std::string, F32> validated;
    for (const std::string& setting : settingNames())
    {
        if (!values.has(setting)) continue;
        F32 value;
        if (!validatedPresetValue(setting, values[setting], value)) return false;
        validated[setting] = value;
    }
    std::map<std::string, LLColor4> validated_colors;
    for (S32 band = 0; band < BAND_COUNT; ++band)
    {
        const std::string setting = selectionColorSettingName((Band)band);
        if (!values.has(setting)) continue;
        LLColor4 color;
        if (!validatedPresetColor(values[setting], color)) return false;
        validated_colors[setting] = color;
    }
    resetAll();
    for (const auto& entry : validated) gSavedSettings.setF32(entry.first, entry.second);
    for (const auto& entry : validated_colors) gSavedSettings.setColor4(entry.first, entry.second);
    gSavedSettings.setString("ASColorGradeLUTFile", lut_file);
    gSavedSettings.setBOOL("ASColorGradeLUTEnabled", lut_enabled);
    gSavedSettings.setBOOL("ASColorGradeNegativeEnabled", negative);
    gSavedSettings.setBOOL("ASColorGradeGrainStatic", grain_static);
    gSavedSettings.setBOOL("ASColorGradeColorizeEnabled", colorize);
    gSavedSettings.setBOOL("ASColorGradeSplitToningEnabled", split_toning);
    return true;
}

bool ASColorGrading::deletePreset(const std::string& name)
{
    if (!validPresetName(name)) return false;
    return LLFile::remove(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml") == 0;
}
