/**
* @file irtualTrackpad.cpp
* @author Angeldark Raymaker; derived from llVirtualTrackball by Andrey Lihatskiy
* @brief Header file for FSVirtualTrackpad
*
* $LicenseInfo:firstyear=2001&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2018, Linden Research, Inc.
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
* Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
* $/LicenseInfo$
*/

// A two dimensional slider control with optional pinch-mode.

#include "fsvirtualtrackpad.h"
#include "llrect.h"
#include "lluictrlfactory.h"
#include "llkeyboard.h"

// Globals
static LLDefaultChildRegistry::Register<FSVirtualTrackpad> register_virtual_trackball("fs_virtual_trackpad");

FSVirtualTrackpad::Params::Params()
  : border("border"),
    image_moon_back("image_moon_back"),
    image_moon_front("image_moon_front"),
    image_sphere("image_sphere"),
    image_sun_back("image_sun_back"),
    image_sun_front("image_sun_front"),
    pinch_mode("pinch_mode"),
    infinite_scroll_mode("infinite_scroll_mode")
{
}

FSVirtualTrackpad::FSVirtualTrackpad(const FSVirtualTrackpad::Params &p)
  : LLUICtrl(p),
    mImgMoonBack(p.image_moon_back),
    mImgMoonFront(p.image_moon_front),
    mImgSunBack(p.image_sun_back),
    mImgSunFront(p.image_sun_front),
    mImgSphere(p.image_sphere),
    _allowPinchMode(p.pinch_mode),
    _infiniteScrollMode(p.infinite_scroll_mode)
{
    LLRect border_rect = getLocalRect();
    _cursorValueX = _pinchCursorValueX = border_rect.getCenterX();
    _cursorValueY = _pinchCursorValueY = border_rect.getCenterY();
    _cursorValueZ = _pinchCursorValueZ = 0;

    _thumbClickOffsetX = _thumbClickOffsetY = _pinchThumbClickOffsetX = _pinchThumbClickOffsetY = 0;
    _posXwhenCtrlDown = _posYwhenCtrlDown = 0;
    _pinchValueDeltaX = _pinchValueDeltaY = _pinchValueDeltaZ = 0;
    _valueDeltaX = _valueDeltaY = _valueDeltaZ = 0;

    LLViewBorder::Params border = p.border;
    border.rect(border_rect);
    mBorder = LLUICtrlFactory::create<LLViewBorder>(border);
    addChild(mBorder);

    LLPanel::Params touch_area;
    touch_area.rect = LLRect(border_rect);
    mTouchArea = LLUICtrlFactory::create<LLPanel>(touch_area);
    addChild(mTouchArea);
}

FSVirtualTrackpad::~FSVirtualTrackpad() {}

bool FSVirtualTrackpad::postBuild()
{
    return true;
}

void FSVirtualTrackpad::drawThumb(bool isPinchThumb)
{
    LLUIImage *thumb;
    if (mTouchArea->isInEnabledChain())
        thumb = isPinchThumb ? mImgSunFront : mImgMoonFront;
    else
        thumb = isPinchThumb ? mImgSunBack : mImgMoonBack;

    S32 x = isPinchThumb ? _pinchCursorValueX : _cursorValueX;
    S32 y = isPinchThumb ? _pinchCursorValueY : _cursorValueY;

    wrapOrClipCursorPosition(&x, &y);

    S32 halfThumbWidth  = thumb->getWidth() / 2;
    S32 halfThumbHeight = thumb->getHeight() / 2;

    thumb->draw(LLRect(x - halfThumbWidth,
                       y + halfThumbHeight,
                       x + halfThumbWidth,
                       y - halfThumbHeight));
}

bool FSVirtualTrackpad::isPointInTouchArea(S32 x, S32 y) const
{
    if (!mTouchArea)
        return false;

    return mTouchArea->getRect().localPointInRect(x, y);
}

void FSVirtualTrackpad::determineThumbClickError(S32 x, S32 y)
{
    if (!mTouchArea)
        return;
    if (!mImgSunFront)
        return;

    _thumbClickOffsetX = 0;
    _thumbClickOffsetY = 0;

    S32 errorX = _cursorValueX;
    S32 errorY = _cursorValueY;
    wrapOrClipCursorPosition(&errorX, &errorY);
    errorX -= x;
    errorY -= y;

    if (fabs(errorX) > mImgSunFront->getWidth() / 2.0)
        return;
    if (fabs(errorY) > mImgSunFront->getHeight() / 2.0)
        return;

    _thumbClickOffsetX = errorX;
    _thumbClickOffsetY = errorY;
}

void FSVirtualTrackpad::updateClickErrorIfInfiniteScrolling()
{
    if (!_infiniteScrollMode)
        return;
    S32 errorX = _cursorValueX;
    S32 errorY = _cursorValueY;

    LLRect rect = mTouchArea->getRect();
    while (errorX > rect.mRight)
    {
        errorX -= rect.getWidth();
        _thumbClickOffsetX += rect.getWidth();
    }
    while (errorX < rect.mLeft)
    {
        errorX += rect.getWidth();
        _thumbClickOffsetX -= rect.getWidth();
    }
    while (errorY > rect.mTop)
    {
        errorY -= rect.getHeight();
        _thumbClickOffsetY += rect.getHeight();
    }
    while (errorY < rect.mBottom)
    {
        errorY += rect.getHeight();
        _thumbClickOffsetY -= rect.getHeight();
    }
}

void FSVirtualTrackpad::determineThumbClickErrorForPinch(S32 x, S32 y)
{
    if (!mTouchArea)
        return;
    if (!mImgMoonFront)
        return;

    _pinchThumbClickOffsetX = 0;
    _pinchThumbClickOffsetY = 0;

    S32 errorX = _pinchCursorValueX;
    S32 errorY = _pinchCursorValueY;
    wrapOrClipCursorPosition(&errorX, &errorY);
    errorX -= x;
    errorY -= y;

    if (fabs(errorX) > mImgMoonFront->getWidth() / 2.0)
        return;
    if (fabs(errorY) > mImgMoonFront->getHeight() / 2.0)
        return;

    _pinchThumbClickOffsetX = errorX;
    _pinchThumbClickOffsetY = errorY;
}

void FSVirtualTrackpad::updateClickErrorIfInfiniteScrollingForPinch()
{
    if (!_infiniteScrollMode)
        return;
    S32 errorX = _cursorValueX;
    S32 errorY = _cursorValueY;

    LLRect rect = mTouchArea->getRect();
    while (errorX > rect.mRight)
    {
        errorX -= rect.getWidth();
        _pinchThumbClickOffsetX += rect.getWidth();
    }
    while (errorX < rect.mLeft)
    {
        errorX += rect.getWidth();
        _pinchThumbClickOffsetX -= rect.getWidth();
    }
    while (errorY > rect.mTop)
    {
        errorY -= rect.getHeight();
        _pinchThumbClickOffsetY += rect.getHeight();
    }
    while (errorY < rect.mBottom)
    {
        errorY += rect.getHeight();
        _pinchThumbClickOffsetY -= rect.getHeight();
    }
}

void FSVirtualTrackpad::draw()
{
    mImgSphere->draw(mTouchArea->getRect(), mTouchArea->isInEnabledChain() ? UI_VERTEX_COLOR : UI_VERTEX_COLOR % 0.5f);

    if (_allowPinchMode)
        drawThumb(true);

    drawThumb(false);

    LLView::draw();
}

void FSVirtualTrackpad::setValue(const LLSD& value)
{
    if (!value.isArray())
        return;

    if (value.size() == 2)
    {
        LLVector2 vec2;
        vec2.setValue(value);
        setValue(vec2.mV[VX], vec2.mV[VY], 0);
    }
    else if (value.size() == 3)
    {
        LLVector3 vec3;
        vec3.setValue(value);
        setValue(vec3.mV[VX], vec3.mV[VY], vec3.mV[VZ]);
    }
}

void FSVirtualTrackpad::setValue(F32 x, F32 y, F32 z)
{
    convertNormalizedToPixelPos(x, y, z, &_cursorValueX, &_cursorValueY, &_cursorValueZ);
    _valueX = _cursorValueX;
    _valueY = _cursorValueY;
    _valueZ = _cursorValueZ;
}

void FSVirtualTrackpad::setPinchValue(F32 x, F32 y, F32 z)
{
    convertNormalizedToPixelPos(x, y, z, &_pinchCursorValueX, &_pinchCursorValueY, &_pinchCursorValueZ);
    _pinchValueX = _pinchCursorValueX;
    _pinchValueY = _pinchCursorValueY;
    _pinchValueZ = _pinchCursorValueZ;
}

LLSD FSVirtualTrackpad::getValue() { return normalizePixelPos(_valueX, _valueY, _valueZ).getValue(); }
LLSD FSVirtualTrackpad::getValueDelta() { return normalizeDelta(_valueDeltaX, _valueDeltaY, _valueDeltaZ).getValue(); }

LLSD FSVirtualTrackpad::getPinchValue() { return normalizePixelPos(_pinchValueX, _pinchValueY, _pinchValueZ).getValue(); }
LLSD FSVirtualTrackpad::getPinchValueDelta() { return normalizeDelta(_pinchValueDeltaX, _pinchValueDeltaY, _pinchValueDeltaZ).getValue(); }

void FSVirtualTrackpad::wrapOrClipCursorPosition(S32* x, S32* y) const
{
    if (!x || !y)
        return;

    LLRect rect = mTouchArea->getRect();
    if (_infiniteScrollMode)
    {
        while (*x > rect.mRight)
            *x -= rect.getWidth();
        while (*x < rect.mLeft)
            *x += rect.getWidth();
        while (*y > rect.mTop)
            *y -= rect.getHeight();
        while (*y < rect.mBottom)
            *y += rect.getHeight();
    }
    else
    {
        rect.clampPointToRect(*x, *y);
    }
}

bool FSVirtualTrackpad::handleHover(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture())
        return true;

    S32 deltaX, deltaY;
    getHoverMovementDeltas(x, y, mask, &deltaX, &deltaY);
    applyHoverMovementDeltas(deltaX, deltaY, mask);
    applyDeltasToValues(deltaX, deltaY, mask);
    applyDeltasToDeltaValues(deltaX, deltaY, mask);

    onCommit();

    return true;
}

void FSVirtualTrackpad::getHoverMovementDeltas(S32 x, S32 y, MASK mask, S32* deltaX, S32* deltaY)
{
    if (!deltaX || !deltaY)
        return;

    S32 fromX, fromY;
    fromX = _doingPinchMode ? _pinchCursorValueX : _cursorValueX;
    fromY = _doingPinchMode ? _pinchCursorValueY : _cursorValueY;

    if (mask & MASK_CONTROL)
    {
        if (!_heldDownControlBefore)
        {
            _posXwhenCtrlDown = x;
            _posYwhenCtrlDown = y;
            _heldDownControlBefore = true;
        }

        if (_doingPinchMode)
        {
            *deltaX = _posXwhenCtrlDown - (_posXwhenCtrlDown - x) / 8 + _pinchThumbClickOffsetX - fromX;
            *deltaY = _posYwhenCtrlDown - (_posYwhenCtrlDown - y) / 8 + _pinchThumbClickOffsetY - fromY;
        }
        else
        {
            *deltaX = _posXwhenCtrlDown - (_posXwhenCtrlDown - x) / 8 + _thumbClickOffsetX - fromX;
            *deltaY = _posYwhenCtrlDown - (_posYwhenCtrlDown - y) / 8 + _thumbClickOffsetY - fromY;
        }
    }
    else
    {
        if (_heldDownControlBefore)
        {
            _thumbClickOffsetX = fromX - x;
            _thumbClickOffsetY = fromY - y;
            _heldDownControlBefore = false;
        }

        if (_doingPinchMode)
        {
            *deltaX = x + _pinchThumbClickOffsetX - fromX;
            *deltaY = y + _pinchThumbClickOffsetY - fromY;
        }
        else
        {
            *deltaX = x + _thumbClickOffsetX - fromX;
            *deltaY = y + _thumbClickOffsetY - fromY;
        }
    }
}

void FSVirtualTrackpad::applyHoverMovementDeltas(S32 deltaX, S32 deltaY, MASK mask)
{
    if (_doingPinchMode)
    {
        _pinchCursorValueX += deltaX;
        _pinchCursorValueY += deltaY;
        if (!_infiniteScrollMode)  // then constrain the cursor within control area
            wrapOrClipCursorPosition(&_pinchCursorValueX, &_pinchCursorValueY);
    }
    else
    {
        _cursorValueX += deltaX;
        _cursorValueY += deltaY;

        if (!_infiniteScrollMode)  // then constrain the cursor within control area
            wrapOrClipCursorPosition(&_cursorValueX, &_cursorValueY);
    }
}

void FSVirtualTrackpad::applyDeltasToValues(S32 deltaX, S32 deltaY, MASK mask)
{
    if (_doingPinchMode)
    {
        if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
        {
            _pinchValueY += deltaY;
            _pinchValueZ += deltaX;
        }
        else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
        {
            _pinchValueX += deltaX;
            _pinchValueZ += deltaY;
        }
        else
        {
            _pinchValueX += deltaX;
            _pinchValueY += deltaY;
        }
    }
    else
    {
        if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
        {
            _valueY += deltaY;
            _valueZ += deltaX;
        }
        else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
        {
            _valueX += deltaX;
            _valueZ += deltaY;
        }
        else
        {
            _valueX += deltaX;
            _valueY += deltaY;
        }
    }
}

void FSVirtualTrackpad::applyDeltasToDeltaValues(S32 deltaX, S32 deltaY, MASK mask)
{
    if (_doingPinchMode)
    {
        if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
        {
            _pinchValueDeltaX = 0;
            _pinchValueDeltaY = deltaY;
            _pinchValueDeltaZ = deltaX;
        }
        else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
        {
            _pinchValueDeltaX = deltaX;
            _pinchValueDeltaY = 0;
            _pinchValueDeltaZ = deltaY;
        }
        else
        {
            _pinchValueDeltaX = deltaX;
            _pinchValueDeltaY = deltaY;
            _pinchValueDeltaZ = 0;
        }
    }
    else
    {
        if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
        {
            _valueDeltaX = 0;
            _valueDeltaY = deltaY;
            _valueDeltaZ = deltaX;
        }
        else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
        {
            _valueDeltaX = deltaX;
            _valueDeltaY = 0;
            _valueDeltaZ = deltaY;
        }
        else
        {
            _valueDeltaX = deltaX;
            _valueDeltaY = deltaY;
            _valueDeltaZ = 0;
        }
    }
}

LLVector3 FSVirtualTrackpad::normalizePixelPos(S32 x, S32 y, S32 z) const
{
    LLVector3 result;
    if (!mTouchArea)
        return result;

    LLRect rect = mTouchArea->getRect();
    S32    centerX = rect.getCenterX();
    S32    centerY = rect.getCenterY();
    S32    width = rect.getWidth();
    S32    height = rect.getHeight();

    result.mV[VX] = (F32) (x - centerX) / width * 2;
    result.mV[VY] = (F32) (y - centerY) / height * 2;
    result.mV[VZ] = (F32) z * ThirdAxisQuantization;

    return result;
}

LLVector3 FSVirtualTrackpad::normalizeDelta(S32 x, S32 y, S32 z) const
{
    LLVector3 result;
    if (!mTouchArea)
        return result;

    LLRect rect = mTouchArea->getRect();
    S32    width = rect.getWidth();
    S32    height = rect.getHeight();

    result.mV[VX] = (F32) x / width * 2;
    result.mV[VY] = (F32) y / height * 2;
    result.mV[VZ] = (F32) z * ThirdAxisQuantization;

    return result;
}

void FSVirtualTrackpad::convertNormalizedToPixelPos(F32 x, F32 y, F32 z, S32 *valX, S32 *valY, S32 *valZ)
{
    if (!mTouchArea)
        return;

    LLRect rect    = mTouchArea->getRect();
    S32    centerX = rect.getCenterX();
    S32    centerY = rect.getCenterY();
    S32    width   = rect.getWidth();
    S32    height  = rect.getHeight();

    if (_infiniteScrollMode)
    {
        *valX = centerX + ll_round(x * width / 2);
        *valY = centerY + ll_round(y * height / 2);
    }
    else
    {
        *valX = centerX + ll_round(llclamp(x, -1, 1) * width / 2);
        *valY = centerY + ll_round(llclamp(y, -1, 1) * height / 2);
    }

    *valZ = ll_round(z / ThirdAxisQuantization);
}

bool FSVirtualTrackpad::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(NULL);
        _heldDownControlBefore = false;
        make_ui_sound("UISndClickRelease");
    }

    return LLView::handleMouseUp(x, y, mask);
}

bool FSVirtualTrackpad::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (isPointInTouchArea(x, y))
    {
        determineThumbClickError(x, y);
        updateClickErrorIfInfiniteScrolling();
        gFocusMgr.setMouseCapture(this);
        make_ui_sound("UISndClick");
    }

    return LLView::handleMouseDown(x, y, mask);
}

bool FSVirtualTrackpad::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        _doingPinchMode = false;
        gFocusMgr.setMouseCapture(NULL);

        make_ui_sound("UISndClickRelease");
    }

    return LLView::handleRightMouseUp(x, y, mask);
}

// move pinch cursor
bool FSVirtualTrackpad::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (!_allowPinchMode)
        return LLView::handleRightMouseDown(x, y, mask);

    if (isPointInTouchArea(x, y))
    {
        determineThumbClickErrorForPinch(x, y);
        updateClickErrorIfInfiniteScrollingForPinch();
        _doingPinchMode = true;
        gFocusMgr.setMouseCapture(this);

        make_ui_sound("UISndClick");
    }

    return LLView::handleRightMouseDown(x, y, mask);
}

// pass wheel-clicks to third axis
bool FSVirtualTrackpad::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (hasMouseCapture() || isPointInTouchArea(x, y))
    {
        MASK mask = gKeyboard->currentMask(true);

        S32 changeAmount = WheelClickQuanta;
        if (mask & MASK_CONTROL)
            changeAmount /= 5;

        if (_doingPinchMode)
        {
            if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
                _pinchValueX -= clicks * changeAmount;
            else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
                _pinchValueY -= clicks * changeAmount;
            else
                _pinchValueZ -= clicks * changeAmount;
        }
        else
        {
            if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_ALT)
                _valueX -= clicks * changeAmount;
            else if ((mask & (MASK_SHIFT | MASK_ALT)) == MASK_SHIFT)
                _valueY -= clicks * changeAmount;
            else
                _valueZ -= clicks * changeAmount;
        }

        if (!hasMouseCapture())
            onCommit();

        return true;
    }
    else
        return LLUICtrl::handleScrollWheel(x, y, clicks);
}
