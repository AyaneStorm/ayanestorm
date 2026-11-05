/**
 * asrtestwindowwin32.cpp
 * Windows implementation of the asrenderer_test window.
 */
#include "asrtestwindow.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace asrtest
{

namespace
{

bool sClosed = false;

LRESULT CALLBACK onMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        sClosed = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool TestWindow::create(uint32_t width, uint32_t height, const char* title)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = onMessage;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"ASRTestWindow";
    RegisterClassW(&windowClass);

    wchar_t wideTitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, 256);

    RECT rect{ 0, 0, LONG(width), LONG(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, wideTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, instance, nullptr);
    mWindow = window;
    return window != nullptr;
}

bool TestWindow::pumpEvents()
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !sClosed;
}

void TestWindow::resize(uint32_t width, uint32_t height)
{
    RECT rect{ 0, 0, LONG(width), LONG(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(static_cast<HWND>(mWindow), nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void TestWindow::pixelSize(uint32_t& width, uint32_t& height) const
{
    RECT rect;
    GetClientRect(static_cast<HWND>(mWindow), &rect);
    width = uint32_t(rect.right);
    height = uint32_t(rect.bottom);
}

asr::SurfaceTarget TestWindow::surfaceTarget() const
{
    asr::SurfaceTarget target;
    target.nativeWindow = mWindow;
    target.nativeInstance = GetModuleHandleW(nullptr);
    return target;
}

bool glContextIsCurrent()
{
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl)
    {
        return false;
    }
    using GetContext = void*(WINAPI*)();
    GetContext getContext = reinterpret_cast<GetContext>(GetProcAddress(gl, "wglGetCurrentContext"));
    return getContext && getContext();
}

} // namespace asrtest
