/**
 * @file ascolorgrading.cpp
 * @author chanayane@firestorm
 * @brief Viewer-local photographic color grading and preset storage.
 */
#include "llviewerprecompiledheaders.h"

#include "ascolorgrading.h"

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

    const char* const BAND_NAMES[ASColorGrading::BAND_COUNT] =
        { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta" };
    const char* const COMPONENTS[3] = { "Hue", "Saturation", "Luminance" };
    const char* const BUILTIN_PRESETS[] =
        { "Neutral", "Sepia", "Cyanotype", "Selenium", "Black & White", "Warm Vintage", "Cool Cinematic", "Bleach Bypass", "Vivid" };

    const LLStaticHashedString sLinearEnabled("as_color_grade_linear_enabled");
    const LLStaticHashedString sExposure("as_color_grade_exposure");
    const LLStaticHashedString sWhiteBalance("as_color_grade_white_balance");
    const LLStaticHashedString sBasic1("as_color_grade_basic1");
    const LLStaticHashedString sBasic2("as_color_grade_basic2");
    const LLStaticHashedString sBasic3("as_color_grade_basic3");
    const LLStaticHashedString sBands("as_color_grade_bands");
    const LLStaticHashedString sColorize("as_color_grade_colorize");
    const LLStaticHashedString sGrain("as_color_grade_grain");
    const LLStaticHashedString sGrainSeed("as_color_grade_grain_seed");
    const LLStaticHashedString sSnapshotTile("as_color_grade_snapshot_tile");

    F32 normalized(const char* name)
    {
        return llclamp(gSavedSettings.getF32(name) * 0.01f, -1.f, 1.f);
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
        std::map<std::string, F32> values;
        if (name == "Sepia")
            values = { {"ASColorGradeColorizeHue",35.f}, {"ASColorGradeColorizeSaturation",25.f},
                {"ASColorGradeColorizeLuminance",0.f}, {"ASColorGradeExposure",.1f},
                {"ASColorGradeBrightness",4.f}, {"ASColorGradeContrast",12.f}, {"ASColorGradeHighlights",-15.f},
                {"ASColorGradeShadows",12.f}, {"ASColorGradeBlacks",-8.f}, {"ASColorGradeGrainAmount",18.f}, {"ASColorGradeGrainSize",45.f},
                {"ASColorGradeGrainRoughness",65.f}, {"ASColorGradeGrainColor",0.f} };
        else if (name == "Cyanotype")
            values = { {"ASColorGradeColorizeHue",215.f}, {"ASColorGradeColorizeSaturation",25.f},
                {"ASColorGradeColorizeLuminance",0.f}, {"ASColorGradeContrast",18.f},
                {"ASColorGradeHighlights",-10.f}, {"ASColorGradeShadows",-8.f}, {"ASColorGradeBlacks",-12.f},
                {"ASColorGradeGrainAmount",12.f}, {"ASColorGradeGrainSize",38.f},
                {"ASColorGradeGrainRoughness",58.f}, {"ASColorGradeGrainColor",0.f} };
        else if (name == "Selenium")
            values = { {"ASColorGradeContrast",21.f}, {"ASColorGradeHighlights",12.f},
                {"ASColorGradeShadows",11.f}, {"ASColorGradeWhites",9.f}, {"ASColorGradeBlacks",2.f},
                {"ASColorGradeTemperature",54.f}, {"ASColorGradeTint",8.f}, {"ASColorGradeVibrance",12.f},
                {"ASColorGradeColorizeHue",218.f}, {"ASColorGradeColorizeSaturation",10.f},
                {"ASColorGradeColorizeLuminance",0.f}, {"ASColorGradeGrainAmount",52.f},
                {"ASColorGradeGrainSize",18.f}, {"ASColorGradeGrainRoughness",56.f},
                {"ASColorGradeGrainColor",10.f} };
        else if (name == "Black & White")
            values = { {"ASColorGradeSaturation",-100.f}, {"ASColorGradeContrast",12.f}, {"ASColorGradeShadows",8.f},
                {"ASColorGradeBlacks",-10.f}, {"ASColorGradeGrainAmount",10.f}, {"ASColorGradeGrainColor",0.f} };
        else if (name == "Warm Vintage")
            values = { {"ASColorGradeTemperature",55.f}, {"ASColorGradeTint",8.f}, {"ASColorGradeContrast",-8.f},
                {"ASColorGradeHighlights",-25.f}, {"ASColorGradeShadows",18.f}, {"ASColorGradeBlacks",8.f},
                {"ASColorGradeSaturation",-18.f}, {"ASColorGradeVibrance",12.f}, {"ASColorGradeGrainAmount",20.f},
                {"ASColorGradeGrainSize",55.f}, {"ASColorGradeGrainRoughness",70.f}, {"ASColorGradeGrainColor",10.f} };
        else if (name == "Cool Cinematic")
            values = { {"ASColorGradeTemperature",-35.f}, {"ASColorGradeTint",-8.f}, {"ASColorGradeContrast",18.f},
                {"ASColorGradeHighlights",-18.f}, {"ASColorGradeShadows",-12.f}, {"ASColorGradeBlacks",-12.f},
                {"ASColorGradeSaturation",-8.f}, {"ASColorGradeVibrance",20.f}, {"ASColorGradeOrangeSaturation",10.f},
                {"ASColorGradeOrangeLuminance",5.f}, {"ASColorGradeAquaSaturation",15.f},
                {"ASColorGradeBlueSaturation",20.f}, {"ASColorGradeBlueLuminance",-8.f} };
        else if (name == "Bleach Bypass")
            values = { {"ASColorGradeSaturation",-65.f}, {"ASColorGradeContrast",35.f}, {"ASColorGradeHighlights",-10.f},
                {"ASColorGradeShadows",-20.f}, {"ASColorGradeBlacks",-25.f}, {"ASColorGradeGrainAmount",16.f},
                {"ASColorGradeGrainRoughness",60.f}, {"ASColorGradeGrainColor",0.f} };
        else if (name == "Vivid")
            values = { {"ASColorGradeContrast",10.f}, {"ASColorGradeSaturation",8.f},
                {"ASColorGradeVibrance",35.f}, {"ASColorGradeHighlights",-8.f}, {"ASColorGradeShadows",8.f} };
        for (const auto& value : values) gSavedSettings.setF32(value.first, value.second);
        if (name == "Sepia" || name == "Cyanotype" || name == "Selenium")
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
        else if (name == "ASColorGradeTemperature") { minimum = -200.f; maximum = 200.f; }
        else if (name == "ASColorGradeTint") { minimum = -200.f; maximum = 200.f; }
        else if (name.find("ASColorGradeGrain") == 0) { minimum = 0.f; maximum = 100.f; }
        output = llclamp(output, minimum, maximum);
        return true;
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
            "ASColorGradeHue", "ASColorGradeGrainAmount", "ASColorGradeGrainSize",
            "ASColorGradeGrainRoughness", "ASColorGradeGrainColor", "ASColorGradeColorizeHue",
            "ASColorGradeColorizeSaturation", "ASColorGradeColorizeLuminance" };
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

void ASColorGrading::registerShaders(std::vector<LLGLSLShader*>& shaders) { shaders.push_back(&sFinalProgram); }

void ASColorGrading::appendLinearShader(LLGLSLShader& shader)
{
    shader.mShaderFiles.emplace_back("deferred/ascolorgradingLinearF.glsl", GL_FRAGMENT_SHADER);
}

bool ASColorGrading::createShaders(S32 shader_level)
{
    sFinalProgram.mName = "AyaneStorm Color Grading Presentation Shader";
    sFinalProgram.mShaderFiles.clear();
    sFinalProgram.clearPermutations();
    sFinalProgram.mFeatures.isDeferred = true;
    sFinalProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sFinalProgram.mShaderFiles.emplace_back("deferred/ascolorgradingF.glsl", GL_FRAGMENT_SHADER);
    sFinalProgram.addPermutation("HAS_NOISE", "1");
    sFinalProgram.mShaderLevel = shader_level;
    return sFinalProgram.createShader();
}

void ASColorGrading::unloadShaders() { sFinalProgram.unload(); }

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
    sFinalProgram.uniform1f(sBasic3, gSavedSettings.getF32("ASColorGradeHue") * DEG_TO_RAD);
    F32 bands[BAND_COUNT * 3];
    for (S32 band = 0; band < BAND_COUNT; ++band)
    {
        bands[band * 3] = gSavedSettings.getF32(bandSettingName((Band)band, "Hue")) * 0.3f * DEG_TO_RAD;
        bands[band * 3 + 1] = normalized(bandSettingName((Band)band, "Saturation").c_str());
        bands[band * 3 + 2] = normalized(bandSettingName((Band)band, "Luminance").c_str());
    }
    sFinalProgram.uniform3fv(sBands, BAND_COUNT, bands);
    sFinalProgram.uniform4f(sColorize, gSavedSettings.getBOOL("ASColorGradeColorizeEnabled") ? 1.f : 0.f,
        llclamp(gSavedSettings.getF32("ASColorGradeColorizeHue"), 0.f, 360.f) * DEG_TO_RAD,
        llclamp(gSavedSettings.getF32("ASColorGradeColorizeSaturation") * .01f, 0.f, 1.f),
        normalized("ASColorGradeColorizeLuminance"));
    sFinalProgram.uniform4f(sGrain, llclamp(gSavedSettings.getF32("ASColorGradeGrainAmount") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainSize") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainRoughness") * .01f, 0.f, 1.f),
        llclamp(gSavedSettings.getF32("ASColorGradeGrainColor") * .01f, 0.f, 1.f));
    static U32 snapshot_seed = 0;
    static bool was_snapshot = false;
    if (gSnapshot && !was_snapshot) snapshot_seed = gFrameCount;
    was_snapshot = gSnapshot;
    sFinalProgram.uniform1f(sGrainSeed, (F32)(gSnapshot ? snapshot_seed : gFrameCount));
    const F32 zoom = llmax(LLViewerCamera::getInstance()->getZoomFactor(), 1.f);
    const S32 tile = LLViewerCamera::getInstance()->getZoomSubRegion();
    const S32 row = llceil(zoom);
    sFinalProgram.uniform3f(sSnapshotTile, zoom, zoom > 1.f ? (F32)(tile % row) : 0.f, zoom > 1.f ? (F32)(tile / row) : 0.f);
    triangle.setBuffer();
    triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    sFinalProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, color.getUsage());
    sFinalProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    sFinalProgram.unbind();
    return true;
}

void ASColorGrading::resetAll()
{
    if (LLControlVariable* control = gSavedSettings.getControl("ASColorGradeColorizeEnabled")) control->resetToDefault(true);
    for (const std::string& name : settingNames())
        if (LLControlVariable* control = gSavedSettings.getControl(name)) control->resetToDefault(true);
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
    return std::find(std::begin(BUILTIN_PRESETS), std::end(BUILTIN_PRESETS), name) != std::end(BUILTIN_PRESETS);
}

bool ASColorGrading::savePreset(const std::string& name)
{
    if (!validPresetName(name)) return false;
    LLSD data; data["version"] = 1; data["name"] = name;
    for (const std::string& setting : settingNames()) data["values"][setting] = gSavedSettings.getLLSD(setting);
    data["values"]["ASColorGradeColorizeEnabled"] = gSavedSettings.getBOOL("ASColorGradeColorizeEnabled");
    llofstream file(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml");
    return file.is_open() && LLSDSerialize::toPrettyXML(data, file);
}

bool ASColorGrading::loadPreset(const std::string& name)
{
    if (isReadOnlyPreset(name)) { applyBuiltinPreset(name); return true; }
    llifstream file(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml");
    LLSD data;
    if (!file.is_open() || LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE ||
        !data.isMap() || data["version"].asInteger() != 1 || !data["values"].isMap()) return false;
    LLSD values = data["values"];
    if (values.has("ASColorGradeColorizeEnabled") && !values["ASColorGradeColorizeEnabled"].isBoolean()) return false;
    const bool colorize = values.has("ASColorGradeColorizeEnabled") && values["ASColorGradeColorizeEnabled"].asBoolean();
    std::map<std::string, F32> validated;
    for (const std::string& setting : settingNames())
    {
        if (!values.has(setting)) continue;
        F32 value;
        if (!validatedPresetValue(setting, values[setting], value)) return false;
        validated[setting] = value;
    }
    resetAll();
    for (const auto& entry : validated) gSavedSettings.setF32(entry.first, entry.second);
    gSavedSettings.setBOOL("ASColorGradeColorizeEnabled", colorize);
    return true;
}

bool ASColorGrading::deletePreset(const std::string& name)
{
    if (!validPresetName(name)) return false;
    return LLFile::remove(presetDir() + gDirUtilp->getDirDelimiter() + LLURI::escape(name) + ".xml") == 0;
}
