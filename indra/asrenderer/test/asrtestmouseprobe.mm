/**
 * asrtestmouseprobe.mm
 * asrenderer_test --mouse-probe, Mac only: finds whether mouse motion arrives while the
 * main thread does not read its events, which is what lets the render thread turn the
 * camera ahead of a slow main loop. It compares GCMouse, which the viewer uses, with a
 * listen-only event tap on a thread of its own. The main thread reads its events for one
 * second, then sleeps through the next without reading them, and so on, while the person
 * running it moves the mouse over the window.
 */
#include "asrmousemotion.h"
#include "asrtestwindow.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace
{

asr::MouseMotion sTapMotion;
CFMachPortRef sTap = nullptr;

CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void*)
{
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        if (sTap)
        {
            CGEventTapEnable(sTap, true);
        }
        return event;
    }
    sTapMotion.add(float(CGEventGetDoubleValueField(event, kCGMouseEventDeltaX)),
                   float(-CGEventGetDoubleValueField(event, kCGMouseEventDeltaY)));
    return event;
}

// How often a source's total was seen to change, sampling every 2 ms.
struct Seen
{
    asr::MouseMotion::Total last = 0;
    unsigned changes[2] = { 0, 0 }; // while reading events, while not
    double distance[2] = { 0.0, 0.0 };

    void sample(const asr::MouseMotion& motion, int phase)
    {
        const asr::MouseMotion::Total total = motion.total();
        if (total != last)
        {
            float dx = 0.f;
            float dy = 0.f;
            asr::MouseMotion::between(last, total, dx, dy);
            distance[phase] += std::abs(dx) + std::abs(dy);
            ++changes[phase];
            last = total;
        }
    }

    void print(const char* name) const
    {
        printf("%-10s while reading events: %5u changes, %8.0f units; while not reading: %5u changes, %8.0f units\n", name,
               changes[0], distance[0], changes[1], distance[1]);
    }
};

} // namespace

int runMouseProbe(double seconds)
{
    asrtest::TestWindow window;
    if (!window.create(640, 360, "AyaneStorm mouse probe: move the mouse over this window"))
    {
        printf("FAIL  cannot create the window\n");
        return 1;
    }

    asr::MouseMotion gcMotion;
    asr::listenToMice(&gcMotion);

    std::atomic<int> tapState{ 0 }; // 0 starting, 1 running, 2 not allowed
    std::atomic<CFRunLoopRef> tapLoop{ nullptr };
    std::thread tapThread([&]()
    {
        const CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged)
                               | CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged);
        sTap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask, tapCallback, nullptr);
        if (!sTap)
        {
            tapState = 2;
            return;
        }
        CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, sTap, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
        CGEventTapEnable(sTap, true);
        tapLoop = CFRunLoopGetCurrent();
        tapState = 1;
        CFRunLoopRun();
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
        CFRelease(source);
        CFRelease(sTap);
        sTap = nullptr;
    });

    using Clock = std::chrono::steady_clock;
    printf("move the mouse in circles over the window, starting now, for %.0f seconds\n", seconds + 2.0);
    const Clock::time_point warmUp = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < warmUp)
    {
        window.pumpEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Seen gc;
    Seen tap;
    gc.last = gcMotion.total();
    tap.last = sTapMotion.total();
    const Clock::time_point start = Clock::now();
    while (Clock::now() - start < std::chrono::duration<double>(seconds))
    {
        const int phase = int(std::chrono::duration<double>(Clock::now() - start).count()) % 2;
        if (phase == 0 && !window.pumpEvents())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gc.sample(gcMotion, phase);
        tap.sample(sTapMotion, phase);
    }

    asr::listenToMice(nullptr);
    if (CFRunLoopRef loop = tapLoop.load())
    {
        CFRunLoopStop(loop);
    }
    tapThread.join();

    gc.print("GCMouse");
    if (tapState == 2)
    {
        printf("event tap  not allowed here\n");
    }
    else
    {
        tap.print("event tap");
    }
    return 0;
}
