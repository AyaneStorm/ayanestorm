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
    enum Band : S32 { RED, ORANGE, YELLOW, GREEN, AQUA, BLUE, PURPLE, MAGENTA, BAND_COUNT };

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
