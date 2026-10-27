/**
 * @file asshaderreload.h
 * @brief AyaneStorm: one viewer-wide shader reload per frame at most.
 * @author chanayane@firestorm
 *
 * Every setting with a shader listener calls LLViewerShaderMgr::setShaders()
 * itself. Code that writes several such settings in a row (graphics level,
 * UI refresh, presets, tuning) reloaded once per setting, each time with a
 * new intermediate set of global defines, so each reload missed the shader
 * cache and compiled every program (7-9 s each on the test machine).
 * While shaders are loaded, requests are now recorded and served by a single
 * reload at the start of the next displayed frame, with the final settings.
 * The current programs stay valid until then. See
 * doc/ayanestorm-graphics-switch-shader-reload-stall.md.
 */

#ifndef AS_SHADER_RELOAD_H
#define AS_SHADER_RELOAD_H

namespace ASShaderReload
{
    // Called first in LLViewerShaderMgr::setShaders(): true when the request
    // was recorded for flush(). Never defers while no shaders are loaded
    // (startup, GL restore), since callers then need them immediately.
    bool defer();
    // Called by display() before any frame work: runs the recorded reload.
    void flush();
}

#endif
