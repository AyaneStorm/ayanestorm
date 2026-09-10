/**
 * @file ascolorgrading.h
 * @author chanayane@firestorm
 * @brief Viewer-local photographic color grading and preset storage.
 */
#ifndef AS_COLOR_GRADING_H
#define AS_COLOR_GRADING_H

#include <string>
#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASColorGrading
{
    // All three rows use perceptual OKLab targets.
    enum Band : S32
    {
        RED, ORANGE, YELLOW, GREEN, AQUA, BLUE, PURPLE, MAGENTA,
        GRAY_1, GRAY_2, GRAY_3, GRAY_4, GRAY_5, GRAY_6, GRAY_7, GRAY_8,
        RED_SKIN_2, RED_SKIN_4, RED_SKIN_6, RED_SKIN_8,
        SKIN_2, SKIN_4, SKIN_6, SKIN_8,
        BAND_COUNT
    };

    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void appendLinearShader(LLGLSLShader& shader);
    void bindLinearUniforms(LLGLSLShader& shader, bool bypass);
    bool present(LLRenderTarget& color, LLRenderTarget& depth, LLVertexBuffer& screen_triangle);

    bool isActive();
    void setPreviewBypass(bool bypass);
    bool getPreviewBypass();
    void resetAll();

    const std::vector<std::string>& settingNames();
    std::string bandSettingName(Band band, const std::string& component);
    std::vector<std::string> listPresets();
    bool isReadOnlyPreset(const std::string& name);
    bool savePreset(const std::string& name);
    bool loadPreset(const std::string& name);
    bool deletePreset(const std::string& name);
}

#endif
