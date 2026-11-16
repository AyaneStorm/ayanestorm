/**
 * @file asrmousemac.mm
 * asrmousemac.mm
 * See listenToMice() in asrmousemotion.h. GCMouse reports the mouse's own motion on a
 * queue of our choosing, so it arrives however busy the main thread is, and it needs no
 * permission. Its y is taken as up positive, as asr::MouseMotion wants.
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
#include "asrmousemotion.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

namespace asr
{

namespace
{

MouseMotion* sMotion = nullptr; // main thread
dispatch_queue_t sQueue = nil;
id sObserver = nil;

// Every mouse calls on the one serial queue, so motion is only ever added from one thread.
void listenTo(GCMouse* mouse)
{
    MouseMotion* motion = sMotion;
    if (!motion)
    {
        mouse.mouseInput.mouseMovedHandler = nil;
        return;
    }
    mouse.handlerQueue = sQueue;
    mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput* input, float dx, float dy) { motion->add(dx, dy); };
}

} // namespace

void listenToMice(MouseMotion* motion)
{
    sMotion = motion;
    if (motion && !sQueue)
    {
        dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        sQueue = dispatch_queue_create("AyaneStorm mouse motion", attributes);
        sObserver = [[NSNotificationCenter defaultCenter] addObserverForName:GCMouseDidConnectNotification object:nil
            queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* note) { listenTo((GCMouse*)note.object); }];
    }
    for (GCMouse* mouse in GCMouse.mice)
    {
        listenTo(mouse);
    }
    if (!motion && sObserver)
    {
        // A handler already queued may still run. The motion it adds to has to outlive this,
        // which the viewer's, a static, does.
        [[NSNotificationCenter defaultCenter] removeObserver:sObserver];
        sObserver = nil;
        sQueue = nil;
    }
}

} // namespace asr
