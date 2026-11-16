/**
 * @file asrviewer.h
 * @brief Connects the viewer to the new renderer (indra/asrenderer).
 *
 * Choosing the renderer happens once at startup, because OpenGL mode and the new
 * renderer differ from the window up. See section 9 of
 * doc/ayanestorm-new-renderer-plan.md. In a build without AS_RENDERER, or with
 * ASRendererBackend at OpenGL, none of this does anything.
 * @author chanayane@ayanestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
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
 *
 * $/LicenseInfo$
 */
#ifndef ASR_VIEWER_H
#define ASR_VIEWER_H

class LLWindow;

namespace asr
{
    class Device;
    struct CameraTurn;
    struct SceneCamera3D;
}

namespace ASRViewer
{
    // Reads ASRendererBackend and probes the machine. Must run before the window
    // exists. If the machine cannot run the new renderer, OpenGL is used.
    void selectBackend();

    // Creates the device and starts the render thread, once the window exists and
    // before the first texture does.
    void start(LLWindow* window);

    // Hands the render thread the newest state. Called from display(). Never waits.
    void publishFrame(LLWindow* window);

    // What is drawn between these two calls is one finished frame of UI. The render
    // thread goes on showing the last one until the next one is published. False
    // means ASRendererUIHz has no new frame due: draw nothing, and publish nothing.
    bool beginUI();
    void publishUI();

    // Stops the render thread and frees the device, before the window goes.
    void stop();

    // Null while the renderer is not running.
    asr::Device* device();

    // The world is drawn from this camera from now on, turned by the mouse as `turn` says.
    void setCamera(const asr::SceneCamera3D& camera, const asr::CameraTurn& turn);

    // Frames shown since the last call, which is what the viewer's FPS counts: the render
    // rate times the time since, so every main loop reports the rate. With OpenGL that is
    // the one frame display() has just drawn.
    F64 takeShownFrames();

    // True if the new renderer was asked for but could not be used.
    bool fellBackToOpenGL();
}

#endif // ASR_VIEWER_H
