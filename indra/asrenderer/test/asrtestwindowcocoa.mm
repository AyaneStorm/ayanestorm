/**
 * asrtestwindowcocoa.mm
 * macOS implementation of the asrenderer_test window.
 */
#include "asrtestwindow.h"

#import <Cocoa/Cocoa.h>
#include <dlfcn.h>

namespace asrtest
{

bool TestWindow::create(uint32_t width, uint32_t height, const char* title)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, width, height)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = [NSString stringWithUTF8String:title];
    window.releasedWhenClosed = NO;
    [window makeKeyAndOrderFront:nil];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
    mWindow = (__bridge_retained void*)window; // kept for the life of the program
    mView = (__bridge void*)window.contentView;
    return true;
}

bool TestWindow::pumpEvents()
{
    @autoreleasepool
    {
        while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])
        {
            [NSApp sendEvent:event];
        }
        return ((__bridge NSWindow*)mWindow).isVisible;
    }
}

void TestWindow::resize(uint32_t width, uint32_t height)
{
    [(__bridge NSWindow*)mWindow setContentSize:NSMakeSize(width, height)];
}

void TestWindow::pixelSize(uint32_t& width, uint32_t& height) const
{
    NSWindow* window = (__bridge NSWindow*)mWindow;
    const NSSize size = window.contentView.bounds.size;
    const CGFloat scale = window.backingScaleFactor;
    width = uint32_t(size.width * scale);
    height = uint32_t(size.height * scale);
}

asr::SurfaceTarget TestWindow::surfaceTarget() const
{
    asr::SurfaceTarget target;
    target.nativeWindow = mView;
    return target;
}

bool glContextIsCurrent()
{
    using GetContext = void* (*)();
    GetContext getContext = reinterpret_cast<GetContext>(dlsym(RTLD_DEFAULT, "CGLGetCurrentContext"));
    return getContext && getContext();
}

} // namespace asrtest
