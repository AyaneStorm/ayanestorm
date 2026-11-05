/**
 * @file asgraphicslevelapply.h
 * @brief AyaneStorm: apply a graphics quality change once the slider settles.
 * @author chanayane@firestorm
 *
 * A quality slider commits on every step while dragged, and each commit runs
 * LLFeatureManager::setGraphicsLevel(), a full shader reload. Dragging across
 * five levels reloaded five times, each a fresh shader cache miss, long enough
 * to lose the region. The quality handlers route through request(), which
 * runs only the latest change once the mouse is released and the value has
 * been stable briefly.
 */

#ifndef AS_GRAPHICS_LEVEL_APPLY_H
#define AS_GRAPHICS_LEVEL_APPLY_H

#include <functional>

namespace ASGraphicsLevelApply
{
    // Replaces any pending change with apply.
    void request(std::function<void()> apply);
    // True while a deferred change runs, so the handler it calls applies
    // instead of deferring again.
    bool applying();
}

#endif
