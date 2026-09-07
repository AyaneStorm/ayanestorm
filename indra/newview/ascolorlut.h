/**
 * @file ascolorlut.h
 * @author chanayane@firestorm
 * @brief Optional display-referred .cube LUTs for color grading.
 */
#ifndef AS_COLOR_LUT_H
#define AS_COLOR_LUT_H

#include <string>

class LLGLSLShader;
class LLPanel;

namespace ASColorLUT
{
    // Presets store a portable basename, never an arbitrary filesystem path.
    bool validName(const std::string& name);
    bool configureShader(LLGLSLShader& shader);
    void bind(LLGLSLShader& shader);
    void unbind();
    void unload();
    void initPanel(LLPanel& panel);
    void refreshPanel(LLPanel& panel);
    void updatePanel(LLPanel& panel);
}

#endif
