/**
 * @file asrgltable.h
 * @brief With the new renderer on, every GL function pointer comes from here.
 *
 * No GL context exists in that mode. LLGLManager::initGL() still runs unchanged:
 * it asks for each function by name, and this table answers. A few functions
 * reply like a driver, so startup succeeds. All others are stubs that do nothing
 * and log their first call, so a call added by an upstream merge reports itself.
 * New function pointers need no list here, because every name is served.
 *
 * See section 1 of doc/ayanestorm-new-renderer-plan.md.
 */
#ifndef ASR_GLTABLE_H
#define ASR_GLTABLE_H

#include <string>

namespace ASR
{
    // True when the new renderer runs this session. Set once, before the window
    // exists, and never changed.
    bool active();
    void setActive(const std::string& vendor, const std::string& renderer);
}

namespace ASRGLTable
{
    // Never null: unknown names get a stub.
    void* get(const char* name);
}

#endif // ASR_GLTABLE_H
