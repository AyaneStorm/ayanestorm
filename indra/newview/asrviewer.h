/**
 * @file asrviewer.h
 * @brief Connects the viewer to the new renderer (indra/asrenderer).
 *
 * Choosing the renderer happens once at startup, because OpenGL mode and the new
 * renderer differ from the window up. See section 9 of
 * doc/ayanestorm-new-renderer-plan.md. In a build without AS_RENDERER, or with
 * ASRendererBackend at OpenGL, none of this does anything.
 */
#ifndef ASR_VIEWER_H
#define ASR_VIEWER_H

class LLWindow;

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

    // True if the new renderer was asked for but could not be used.
    bool fellBackToOpenGL();
}

#endif // ASR_VIEWER_H
