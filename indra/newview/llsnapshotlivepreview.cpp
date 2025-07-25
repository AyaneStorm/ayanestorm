/**
* @file llsnapshotlivepreview.cpp
* @brief Implementation of llsnapshotlivepreview
* @author Gilbert@lindenlab.com
*
* $LicenseInfo:firstyear=2013&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2014, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llagentui.h"
#include "llfilesystem.h"
#include "llcombobox.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llfloaterflickr.h" // <FS:Ansariel> Share to Flickr
#include "fsfloaterprimfeed.h" // <FS:Beq> Share to Primfeed
#include "llimagefilter.h"
#include "llimagefiltersmanager.h"
#include "llimagebmp.h"
#include "llimagej2c.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llimagewebp.h"
#include "lllandmarkactions.h"
#include "lllocalcliprect.h"
#include "llresmgr.h"
#include "llnotificationsutil.h"
#include "llslurl.h"
#include "llsnapshotlivepreview.h"
#include "lltoolfocus.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"   // upload_new_resource()
#include "llviewerstats.h"
#include "llviewertexturelist.h"
#include "llwindow.h"
#include "llworld.h"
#include <boost/filesystem.hpp>

constexpr F32 AUTO_SNAPSHOT_TIME_DELAY = 1.f;

constexpr F32 SHINE_TIME = 0.5f;
constexpr F32 SHINE_WIDTH = 0.6f;
constexpr F32 SHINE_OPACITY = 0.3f;
constexpr F32 FALL_TIME = 0.6f;
constexpr S32 BORDER_WIDTH = 6;
constexpr S32 TOP_PANEL_HEIGHT = 30;

constexpr S32 MAX_TEXTURE_SIZE = 2048 ; //max upload texture size 2048 * 2048

std::set<LLSnapshotLivePreview*> LLSnapshotLivePreview::sList;
LLPointer<LLImageFormatted> LLSnapshotLivePreview::sSaveLocalImage = nullptr;

LLSnapshotLivePreview::LLSnapshotLivePreview (const LLSnapshotLivePreview::Params& p)
    :   LLView(p),
    mColor(1.f, 0.f, 0.f, 0.5f),
    mCurImageIndex(0),
    mPreviewImage(NULL),
    mThumbnailImage(NULL) ,
    mBigThumbnailImage(NULL) ,
    mThumbnailWidth(0),
    mThumbnailHeight(0),
    mThumbnailSubsampled(false),
    mPreviewImageEncoded(NULL),
    mFormattedImage(NULL),
    mShineCountdown(0),
    mFlashAlpha(0.f),
    mNeedsFlash(true),
    mSnapshotQuality(gSavedSettings.getS32("SnapshotQuality")),
    mDataSize(0),
    mSnapshotType(LLSnapshotModel::SNAPSHOT_POSTCARD),
    mSnapshotFormat(LLSnapshotModel::ESnapshotFormat(gSavedSettings.getS32("SnapshotFormat"))),
    mSnapshotUpToDate(false),
    mCameraPos(LLViewerCamera::getInstance()->getOrigin()),
    mCameraRot(LLViewerCamera::getInstance()->getQuaternion()),
    mSnapshotActive(false),
    mSnapshotBufferType(LLSnapshotModel::SNAPSHOT_TYPE_COLOR),
    mFilterName(""),
    mAllowRenderUI(true),
    mAllowFullScreenPreview(true),
    mViewContainer(NULL),
    mFixedThumbnailWidth(0),
    mFixedThumbnailHeight(0),
    mFixedThumbnailImageWidth(0),
    mFixedThumbnailImageHeight(0)
{
    setSnapshotQuality(gSavedSettings.getS32("SnapshotQuality"));
    mSnapshotDelayTimer.setTimerExpirySec(0.0f);
    mSnapshotDelayTimer.start();
    //  gIdleCallbacks.addFunction( &LLSnapshotLivePreview::onIdle, (void*)this );
    sList.insert(this);
    setFollowsAll();
    mWidth[0] = gViewerWindow->getWindowWidthRaw();
    mWidth[1] = gViewerWindow->getWindowWidthRaw();
    mHeight[0] = gViewerWindow->getWindowHeightRaw();
    mHeight[1] = gViewerWindow->getWindowHeightRaw();
    mImageScaled[0] = false;
    mImageScaled[1] = false;

    mMaxImageSize = MAX_SNAPSHOT_IMAGE_SIZE ;
    mKeepAspectRatio = gSavedSettings.getBOOL("KeepAspectForSnapshot") ;
    mThumbnailUpdateLock = false ;
    mThumbnailUpToDate   = false ;
    mBigThumbnailUpToDate = false ;

    mForceUpdateSnapshot = false;
}

LLSnapshotLivePreview::~LLSnapshotLivePreview()
{
    // delete images
    mPreviewImage = NULL;
    mPreviewImageEncoded = NULL;
    mFormattedImage = NULL;

    //  gIdleCallbacks.deleteFunction( &LLSnapshotLivePreview::onIdle, (void*)this );
    sList.erase(this);
    sSaveLocalImage = NULL;
}

void LLSnapshotLivePreview::setMaxImageSize(S32 size)
{
    mMaxImageSize = llmin(size,(S32)(MAX_SNAPSHOT_IMAGE_SIZE));
}

LLViewerTexture* LLSnapshotLivePreview::getCurrentImage()
{
    return mViewerImage[mCurImageIndex];
}

F32 LLSnapshotLivePreview::getImageAspect()
{
    if (!getCurrentImage())
    {
        return 0.f;
    }
    // mKeepAspectRatio) == gSavedSettings.getBOOL("KeepAspectForSnapshot"))
    return (mKeepAspectRatio ? ((F32)getRect().getWidth()) / ((F32)getRect().getHeight()) : ((F32)getWidth()) / ((F32)getHeight()));
}

void LLSnapshotLivePreview::updateSnapshot(bool new_snapshot, bool new_thumbnail, F32 delay)
{
    LL_DEBUGS("Snapshot") << "updateSnapshot: mSnapshotUpToDate = " << getSnapshotUpToDate() << LL_ENDL;

    // Update snapshot if requested.
    if (new_snapshot)
    {
        if (getSnapshotUpToDate())
        {
            S32 old_image_index = mCurImageIndex;
            mCurImageIndex = (mCurImageIndex + 1) % 2;
            setSize(mWidth[old_image_index], mHeight[old_image_index]);
            mFallAnimTimer.start();
        }
        mSnapshotUpToDate = false;

        // Update snapshot source rect depending on whether we keep the aspect ratio.
        LLRect& rect = mImageRect[mCurImageIndex];
        rect.set(0, getRect().getHeight(), getRect().getWidth(), 0);

        F32 image_aspect_ratio = ((F32)getWidth()) / ((F32)getHeight());
        F32 window_aspect_ratio = ((F32)getRect().getWidth()) / ((F32)getRect().getHeight());

        if (mKeepAspectRatio)//gSavedSettings.getBOOL("KeepAspectForSnapshot"))
        {
            if (image_aspect_ratio > window_aspect_ratio)
            {
                // trim off top and bottom
                S32 new_height = ll_round((F32)getRect().getWidth() / image_aspect_ratio);
                rect.mBottom += (getRect().getHeight() - new_height) / 2;
                rect.mTop -= (getRect().getHeight() - new_height) / 2;
            }
            else if (image_aspect_ratio < window_aspect_ratio)
            {
                // trim off left and right
                S32 new_width = ll_round((F32)getRect().getHeight() * image_aspect_ratio);
                rect.mLeft += (getRect().getWidth() - new_width) / 2;
                rect.mRight -= (getRect().getWidth() - new_width) / 2;
            }
        }

        // Stop shining animation.
        mShineAnimTimer.stop();
        mSnapshotDelayTimer.start();
        mSnapshotDelayTimer.resetWithExpiry(delay);


        mPosTakenGlobal = gAgentCamera.getCameraPositionGlobal();

        // Tell the floater container that the snapshot is in the process of updating itself
        if (mViewContainer)
        {
            mViewContainer->notify(LLSD().with("snapshot-updating", true));
        }
    }

    // Update thumbnail if requested.
    if (new_thumbnail)
    {
        mThumbnailUpToDate = false ;
        mBigThumbnailUpToDate = false;
    }
}

// Return true if the quality has been changed, false otherwise
bool LLSnapshotLivePreview::setSnapshotQuality(S32 quality, bool set_by_user)
{
    llclamp(quality, 0, 100);
    if (quality != mSnapshotQuality)
    {
        mSnapshotQuality = quality;
        if (set_by_user)
        {
            gSavedSettings.setS32("SnapshotQuality", quality);
        }
        mFormattedImage = NULL;     // Invalidate the already formatted image if any
        return true;
    }
    return false;
}

void LLSnapshotLivePreview::drawPreviewRect(S32 offset_x, S32 offset_y, LLColor4 alpha_color)
{
    F32 line_width ;
    glGetFloatv(GL_LINE_WIDTH, &line_width) ;
    gGL.setLineWidth(2.0f * line_width) ; // <FS> Line width OGL core profile fix by Rye Mutt
    LLColor4 color(0.0f, 0.0f, 0.0f, 1.0f) ;
    gl_rect_2d( mPreviewRect.mLeft + offset_x, mPreviewRect.mTop + offset_y,
        mPreviewRect.mRight + offset_x, mPreviewRect.mBottom + offset_y, color, false ) ;
    gGL.setLineWidth(line_width) ; // <FS> Line width OGL core profile fix by Rye Mutt

    //draw four alpha rectangles to cover areas outside of the snapshot image
    if(!mKeepAspectRatio)
    {
        S32 dwl = 0, dwr = 0 ;
        if(mThumbnailWidth > mPreviewRect.getWidth())
        {
            dwl = (mThumbnailWidth - mPreviewRect.getWidth()) >> 1 ;
            dwr = mThumbnailWidth - mPreviewRect.getWidth() - dwl ;

            gl_rect_2d(mPreviewRect.mLeft + offset_x - dwl, mPreviewRect.mTop + offset_y,
                mPreviewRect.mLeft + offset_x, mPreviewRect.mBottom + offset_y, alpha_color, true ) ;
            gl_rect_2d( mPreviewRect.mRight + offset_x, mPreviewRect.mTop + offset_y,
                mPreviewRect.mRight + offset_x + dwr, mPreviewRect.mBottom + offset_y, alpha_color, true ) ;
        }

        if(mThumbnailHeight > mPreviewRect.getHeight())
        {
            S32 dh = (mThumbnailHeight - mPreviewRect.getHeight()) >> 1 ;
            gl_rect_2d(mPreviewRect.mLeft + offset_x - dwl, mPreviewRect.mBottom + offset_y ,
                mPreviewRect.mRight + offset_x + dwr, mPreviewRect.mBottom + offset_y - dh, alpha_color, true ) ;

            dh = mThumbnailHeight - mPreviewRect.getHeight() - dh ;
            gl_rect_2d( mPreviewRect.mLeft + offset_x - dwl, mPreviewRect.mTop + offset_y + dh,
                mPreviewRect.mRight + offset_x + dwr, mPreviewRect.mTop + offset_y, alpha_color, true ) ;
        }
    }
}

//called when the frame is frozen.
void LLSnapshotLivePreview::draw()
{
    if (getCurrentImage() &&
        mPreviewImageEncoded.notNull() &&
        getSnapshotUpToDate())
    {
        LLColor4 bg_color(0.f, 0.f, 0.3f, 0.4f);
        gl_rect_2d(getRect(), bg_color);
        const LLRect& rect = getImageRect();
        LLRect shadow_rect = rect;
        shadow_rect.stretch(BORDER_WIDTH);
        gl_drop_shadow(shadow_rect.mLeft, shadow_rect.mTop, shadow_rect.mRight, shadow_rect.mBottom, LLColor4(0.f, 0.f, 0.f, mNeedsFlash ? 0.f :0.5f), 10);

        LLColor4 image_color(1.f, 1.f, 1.f, 1.f);
        gGL.color4fv(image_color.mV);
        gGL.getTexUnit(0)->bind(getCurrentImage());
        // calculate UV scale
        F32 uv_width = isImageScaled() ? 1.f : llmin((F32)getWidth() / (F32)getCurrentImage()->getWidth(), 1.f);
        F32 uv_height = isImageScaled() ? 1.f : llmin((F32)getHeight() / (F32)getCurrentImage()->getHeight(), 1.f);
        gGL.pushMatrix();
        {
            gGL.translatef((F32)rect.mLeft, (F32)rect.mBottom + TOP_PANEL_HEIGHT, 0.f);
            gGL.begin(LLRender::TRIANGLES);
            {
                gGL.texCoord2f(uv_width, uv_height);
                gGL.vertex2i(rect.getWidth(), rect.getHeight());

                gGL.texCoord2f(0.f, uv_height);
                gGL.vertex2i(0, rect.getHeight());

                gGL.texCoord2f(0.f, 0.f);
                gGL.vertex2i(0, 0);


                gGL.texCoord2f(uv_width, uv_height);
                gGL.vertex2i(rect.getWidth(), rect.getHeight());

                gGL.texCoord2f(0.f, 0.f);
                gGL.vertex2i(0, 0);

                gGL.texCoord2f(uv_width, 0.f);
                gGL.vertex2i(rect.getWidth(), 0);
            }
            gGL.end();
        }
        gGL.popMatrix();

        gGL.color4f(1.f, 1.f, 1.f, mFlashAlpha);
        gl_rect_2d(getRect());
        if (mNeedsFlash)
        {
            if (mFlashAlpha < 1.f)
            {
                mFlashAlpha = lerp(mFlashAlpha, 1.f, LLCriticalDamp::getInterpolant(0.02f));
            }
            else
            {
                mNeedsFlash = false;
            }
        }
        else
        {
            mFlashAlpha = lerp(mFlashAlpha, 0.f, LLCriticalDamp::getInterpolant(0.15f));
        }

        // Draw shining animation if appropriate.
        if (mShineCountdown > 0)
        {
            mShineCountdown--;
            if (mShineCountdown == 0)
            {
                mShineAnimTimer.start();
            }
        }
        else if (mShineAnimTimer.getStarted())
        {
            LL_DEBUGS("Snapshot") << "Drawing shining animation" << LL_ENDL;
            F32 shine_interp = llmin(1.f, mShineAnimTimer.getElapsedTimeF32() / SHINE_TIME);

            // draw "shine" effect
            LLRect local_rect(0, getRect().getHeight() + TOP_PANEL_HEIGHT, getRect().getWidth(), 0);
            LLLocalClipRect clip(local_rect);
            {
                // draw diagonal stripe with gradient that passes over screen
                S32 x1 = gViewerWindow->getWindowWidthScaled() * ll_round((clamp_rescale(shine_interp, 0.f, 1.f, -1.f - SHINE_WIDTH, 1.f)));
                S32 x2 = x1 + ll_round(gViewerWindow->getWindowWidthScaled() * SHINE_WIDTH);
                S32 x3 = x2 + ll_round(gViewerWindow->getWindowWidthScaled() * SHINE_WIDTH);
                S32 y1 = 0;
                S32 y2 = gViewerWindow->getWindowHeightScaled() + TOP_PANEL_HEIGHT;

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.begin(LLRender::TRIANGLES);
                {
                    gGL.color4f(1.f, 1.f, 1.f, 0.f);
                    gGL.vertex2i(x1, y1);
                    gGL.vertex2i(x1 + gViewerWindow->getWindowWidthScaled(), y2);
                    gGL.color4f(1.f, 1.f, 1.f, SHINE_OPACITY);
                    gGL.vertex2i(x2 + gViewerWindow->getWindowWidthScaled(), y2);

                    gGL.color4f(1.f, 1.f, 1.f, 0.f);
                    gGL.vertex2i(x1, y1);
                    gGL.color4f(1.f, 1.f, 1.f, SHINE_OPACITY);
                    gGL.vertex2i(x2 + gViewerWindow->getWindowWidthScaled(), y2);
                    gGL.vertex2i(x2, y1);


                    gGL.color4f(1.f, 1.f, 1.f, SHINE_OPACITY);
                    gGL.vertex2i(x2, y1);
                    gGL.vertex2i(x2 + gViewerWindow->getWindowWidthScaled(), y2);
                    gGL.color4f(1.f, 1.f, 1.f, 0.f);
                    gGL.vertex2i(x3 + gViewerWindow->getWindowWidthScaled(), y2);

                    gGL.color4f(1.f, 1.f, 1.f, SHINE_OPACITY);
                    gGL.vertex2i(x2, y1);
                    gGL.color4f(1.f, 1.f, 1.f, 0.f);
                    gGL.vertex2i(x3 + gViewerWindow->getWindowWidthScaled(), y2);
                    gGL.vertex2i(x3, y1);
                }
                gGL.end();
            }

            // if we're at the end of the animation, stop
            if (shine_interp >= 1.f)
            {
                mShineAnimTimer.stop();
            }
        }
    }

    // draw old image dropping away
    if (mFallAnimTimer.getStarted())
    {
        S32 old_image_index = (mCurImageIndex + 1) % 2;
        if (mViewerImage[old_image_index].notNull() && mFallAnimTimer.getElapsedTimeF32() < FALL_TIME)
        {
            LL_DEBUGS("Snapshot") << "Drawing fall animation" << LL_ENDL;
            F32 fall_interp = mFallAnimTimer.getElapsedTimeF32() / FALL_TIME;
            F32 alpha = clamp_rescale(fall_interp, 0.f, 1.f, 0.8f, 0.4f);
            LLColor4 image_color(1.f, 1.f, 1.f, alpha);
            gGL.color4fv(image_color.mV);
            gGL.getTexUnit(0)->bind(mViewerImage[old_image_index]);
            // calculate UV scale
            // *FIX get this to work with old image
            bool rescale = !mImageScaled[old_image_index] && mViewerImage[mCurImageIndex].notNull();
            F32 uv_width = rescale ? llmin((F32)mWidth[old_image_index] / (F32)mViewerImage[mCurImageIndex]->getWidth(), 1.f) : 1.f;
            F32 uv_height = rescale ? llmin((F32)mHeight[old_image_index] / (F32)mViewerImage[mCurImageIndex]->getHeight(), 1.f) : 1.f;
            gGL.pushMatrix();
            {
                LLRect& rect = mImageRect[old_image_index];
                gGL.translatef((F32)rect.mLeft, (F32)rect.mBottom - ll_round(getRect().getHeight() * 2.f * (fall_interp * fall_interp)), 0.f);
                gGL.rotatef(-45.f * fall_interp, 0.f, 0.f, 1.f);
                gGL.begin(LLRender::TRIANGLES);
                {
                    gGL.texCoord2f(uv_width, uv_height);
                    gGL.vertex2i(rect.getWidth(), rect.getHeight());

                    gGL.texCoord2f(0.f, uv_height);
                    gGL.vertex2i(0, rect.getHeight());

                    gGL.texCoord2f(0.f, 0.f);
                    gGL.vertex2i(0, 0);

                    gGL.texCoord2f(uv_width, uv_height);
                    gGL.vertex2i(rect.getWidth(), rect.getHeight());

                    gGL.texCoord2f(0.f, 0.f);
                    gGL.vertex2i(0, 0);

                    gGL.texCoord2f(uv_width, 0.f);
                    gGL.vertex2i(rect.getWidth(), 0);
                }
                gGL.end();
            }
            gGL.popMatrix();
        }
    }
}

/*virtual*/
void LLSnapshotLivePreview::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLRect old_rect = getRect();
    LLView::reshape(width, height, called_from_parent);
    if (old_rect.getWidth() != width || old_rect.getHeight() != height)
    {
        LL_DEBUGS("Window", "Snapshot") << "window reshaped, updating thumbnail" << LL_ENDL;
        if (mViewContainer && mViewContainer->isInVisibleChain())
        {
            // We usually resize only on window reshape, so give it a chance to redraw, assign delay
            updateSnapshot(
                true, // new snapshot is needed
                false, // thumbnail will be updated either way.
                AUTO_SNAPSHOT_TIME_DELAY); // shutter delay.
        }
    }
}

bool LLSnapshotLivePreview::setThumbnailImageSize()
{
    if (getWidth() < 10 || getHeight() < 10)
    {
        return false ;
    }
    S32 width  = (mThumbnailSubsampled ? mPreviewImage->getWidth()  : gViewerWindow->getWindowWidthRaw());
    S32 height = (mThumbnailSubsampled ? mPreviewImage->getHeight() : gViewerWindow->getWindowHeightRaw()) ;

    F32 aspect_ratio = ((F32)width) / ((F32)height);

    // UI size for thumbnail
    S32 max_width  = mThumbnailPlaceholderRect.getWidth();
    S32 max_height = mThumbnailPlaceholderRect.getHeight();

    if (aspect_ratio > (F32)max_width / (F32)max_height)
    {
        // image too wide, shrink to width
        mThumbnailWidth = max_width;
        mThumbnailHeight = ll_round((F32)max_width / aspect_ratio);
    }
    else
    {
        // image too tall, shrink to height
        mThumbnailHeight = max_height;
        mThumbnailWidth = ll_round((F32)max_height * aspect_ratio);
    }

    if (mThumbnailWidth > width || mThumbnailHeight > height)
    {
        return false ;//if the window is too small, ignore thumbnail updating.
    }

    // <FS:Ansariel> Show miniature thumbnail on collapsed snapshot panel
    if (mFixedThumbnailWidth > 0 && mFixedThumbnailHeight > 0)
    {
        if (aspect_ratio > (F32)mFixedThumbnailWidth / (F32)mFixedThumbnailHeight)
        {
            // image too wide, shrink to width
            mFixedThumbnailImageWidth = mFixedThumbnailWidth;
            mFixedThumbnailImageHeight = ll_round((F32)mFixedThumbnailWidth / aspect_ratio);
        }
        else
        {
            // image too tall, shrink to height
            mFixedThumbnailImageHeight = mFixedThumbnailHeight;
            mFixedThumbnailImageWidth = ll_round((F32)mFixedThumbnailHeight * aspect_ratio);
        }
    }
    // </FS:Ansariel>

    S32 left = 0 , top = mThumbnailHeight, right = mThumbnailWidth, bottom = 0 ;
    if (!mKeepAspectRatio)
    {
        F32 ratio_x = (F32)getWidth()  / width ;
        F32 ratio_y = (F32)getHeight() / height ;

        if (ratio_x > ratio_y)
        {
            top = (S32)(top * ratio_y / ratio_x) ;
        }
        else
        {
            right = (S32)(right * ratio_x / ratio_y) ;
        }
        left = (S32)((mThumbnailWidth - right) * 0.5f) ;
        bottom = (S32)((mThumbnailHeight - top) * 0.5f) ;
        top += bottom ;
        right += left ;
    }
    mPreviewRect.set(left - 1, top + 1, right + 1, bottom - 1) ;

    return true ;
}

void LLSnapshotLivePreview::generateThumbnailImage(bool force_update)
{
    if(mThumbnailUpdateLock) //in the process of updating
    {
        return ;
    }
    if(getThumbnailUpToDate() && !force_update)//already updated
    {
        return ;
    }
    if(getWidth() < 10 || getHeight() < 10)
    {
        return ;
    }

    ////lock updating
    mThumbnailUpdateLock = true ;

    if(!setThumbnailImageSize())
    {
        mThumbnailUpdateLock = false ;
        mThumbnailUpToDate = true ;
        return ;
    }

    // Invalidate the big thumbnail when we regenerate the small one
    mBigThumbnailUpToDate = false;

    if(mThumbnailImage)
    {
        resetThumbnailImage() ;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw;

    if (mThumbnailSubsampled)
    {
        // The thumbnail is be a subsampled version of the preview (used in SL Share previews, i.e. Flickr, Twitter)
        raw->resize( mPreviewImage->getWidth(),
                     mPreviewImage->getHeight(),
                     mPreviewImage->getComponents());
        raw->copy(mPreviewImage);
        // Scale to the thumbnail size
        if (!raw->scale(mThumbnailWidth, mThumbnailHeight))
        {
            raw = NULL ;
        }
    }
    else
    {
        // <FS:Ansariel> Show miniature thumbnail on collapsed snapshot panel
        S32 width = mThumbnailWidth;
        S32 height = mThumbnailHeight;
        if (mFixedThumbnailImageWidth > 0 && mFixedThumbnailImageHeight > 0)
        {
            width = mFixedThumbnailImageWidth;
            height = mFixedThumbnailImageHeight;
        }
        // </FS:Ansariel>

        // The thumbnail is a screen view with screen grab positioning preview
        if(!gViewerWindow->thumbnailSnapshot(raw,
                                         // <FS:Ansariel> Show miniature thumbnail on collapsed snapshot panel
                                         //mThumbnailWidth, mThumbnailHeight,
                                         width, height,
                                         // </FS:Ansariel>
                                         mAllowRenderUI && gSavedSettings.getBOOL("RenderUIInSnapshot"),
                                         gSavedSettings.getBOOL("RenderHUDInSnapshot"),
                                         false,
                                         gSavedSettings.getBOOL("RenderSnapshotNoPost"),
                                         mSnapshotBufferType) )
        {
            raw = NULL ;
        }
    }

    if (raw)
    {
        // Filter the thumbnail
        // Note: filtering needs to be done *before* the scaling to power of 2 or the effect is distorted
        if (getFilter() != "")
        {
            std::string filter_path = LLImageFiltersManager::getInstance()->getFilterPath(getFilter());
            if (filter_path != "")
            {
                LLImageFilter filter(filter_path);
                filter.executeFilter(raw);
            }
            else
            {
                LL_WARNS("Snapshot") << "Couldn't find a path to the following filter : " << getFilter() << LL_ENDL;
            }
        }
        // Scale to a power of 2 so it can be mapped to a texture
        raw->expandToPowerOfTwo();
        mThumbnailImage = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        mThumbnailUpToDate = true ;
    }

    //unlock updating
    mThumbnailUpdateLock = false ;
}

LLViewerTexture* LLSnapshotLivePreview::getBigThumbnailImage()
{
    if (mThumbnailUpdateLock | !mPreviewImage) //in the process of updating <FS:Beq/> (bugsplat avoidance) ensure mPreviewImage is valid
    {
        return NULL;
    }
    if (mBigThumbnailUpToDate && mBigThumbnailImage)//already updated
    {
        return mBigThumbnailImage;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw;

    if (raw)
    {
        // The big thumbnail is a new filtered version of the preview (used in SL Share previews, i.e. Flickr, Twitter)
        mBigThumbnailWidth = mPreviewImage->getWidth();
        mBigThumbnailHeight = mPreviewImage->getHeight();
        raw->resize( mBigThumbnailWidth,
                     mBigThumbnailHeight,
                     mPreviewImage->getComponents());
        raw->copy(mPreviewImage);

        // Filter
        // Note: filtering needs to be done *before* the scaling to power of 2 or the effect is distorted
        if (getFilter() != "")
        {
            std::string filter_path = LLImageFiltersManager::getInstance()->getFilterPath(getFilter());
            if (filter_path != "")
            {
                LLImageFilter filter(filter_path);
                filter.executeFilter(raw);
            }
            else
            {
                LL_WARNS("Snapshot") << "Couldn't find a path to the following filter : " << getFilter() << LL_ENDL;
            }
        }
        // Scale to a power of 2 so it can be mapped to a texture
        raw->expandToPowerOfTwo();
        mBigThumbnailImage = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        mBigThumbnailUpToDate = true ;
    }

    return mBigThumbnailImage ;
}

// Called often. Checks whether it's time to grab a new snapshot and if so, does it.
// Returns true if new snapshot generated, false otherwise.
//static
bool LLSnapshotLivePreview::onIdle( void* snapshot_preview )
{
    LLSnapshotLivePreview* previewp = (LLSnapshotLivePreview*)snapshot_preview;
    if (previewp->getWidth() == 0 || previewp->getHeight() == 0)
    {
        LL_WARNS("Snapshot") << "Incorrect dimensions: " << previewp->getWidth() << "x" << previewp->getHeight() << LL_ENDL;
        return false;
    }

    if (previewp->mSnapshotDelayTimer.getStarted()) // Wait for a snapshot delay timer
    {
        if (!previewp->mSnapshotDelayTimer.hasExpired())
        {
            return false;
        }
        previewp->mSnapshotDelayTimer.stop();
    }

    if (LLToolCamera::getInstance()->hasMouseCapture()) // Hide full-screen preview while camming, either don't take snapshots while ALT-zoom active
    {
        previewp->setVisible(false);
        return false;
    }

    static LLCachedControl<bool> auto_snapshot(gSavedSettings, "AutoSnapshot", false);
    static LLCachedControl<bool> freeze_time(gSavedSettings, "FreezeTime", false);
    static LLCachedControl<bool> use_freeze_frame(gSavedSettings, "UseFreezeFrame", false);
    static LLCachedControl<bool> render_ui(gSavedSettings, "RenderUIInSnapshot", false);
    static LLCachedControl<bool> render_balance(gSavedSettings, "RenderBalanceInSnapshot", false);
    static LLCachedControl<bool> render_hud(gSavedSettings, "RenderHUDInSnapshot", false);
    static LLCachedControl<bool> render_no_post(gSavedSettings, "RenderSnapshotNoPost", false);

    // If we're in freeze-frame and/or auto update mode and camera has moved, update snapshot.
    LLVector3 new_camera_pos = LLViewerCamera::getInstance()->getOrigin();
    LLQuaternion new_camera_rot = LLViewerCamera::getInstance()->getQuaternion();
    if (previewp->mForceUpdateSnapshot ||
        (((auto_snapshot && LLView::isAvailable(previewp->mViewContainer)) ||
        (freeze_time && previewp->mAllowFullScreenPreview)) &&
        (new_camera_pos != previewp->mCameraPos || dot(new_camera_rot, previewp->mCameraRot) < 0.995f)))
    {
        previewp->mCameraPos = new_camera_pos;
        previewp->mCameraRot = new_camera_rot;
        // request a new snapshot whenever the camera moves, with a time delay
        bool new_snapshot = auto_snapshot || previewp->mForceUpdateSnapshot;
        LL_DEBUGS("Snapshot") << "camera moved, updating thumbnail" << LL_ENDL;
        previewp->updateSnapshot(
            new_snapshot, // whether a new snapshot is needed or merely invalidate the existing one
            false, // or if 1st arg is false, whether to produce a new thumbnail image.
            new_snapshot ? AUTO_SNAPSHOT_TIME_DELAY : 0.f); // shutter delay if 1st arg is true.
        previewp->mForceUpdateSnapshot = false;
    }

    if (previewp->getSnapshotUpToDate() && previewp->getThumbnailUpToDate())
    {
        return false;
    }

    // time to produce a snapshot
    if(!previewp->getSnapshotUpToDate())
    {
        LL_DEBUGS("Snapshot") << "producing snapshot" << LL_ENDL;
        if (!previewp->mPreviewImage)
        {
            previewp->mPreviewImage = new LLImageRaw;
        }

        previewp->mSnapshotActive = true;

        previewp->setVisible(false);
        previewp->setEnabled(false);

        previewp->getWindow()->incBusyCount();
        previewp->setImageScaled(false);

        // grab the raw image
        if (gViewerWindow->rawSnapshot(
                previewp->mPreviewImage,
                previewp->getWidth(),
                previewp->getHeight(),
                previewp->mKeepAspectRatio,//gSavedSettings.getBOOL("KeepAspectForSnapshot"),
                previewp->getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE,
                previewp->mAllowRenderUI && render_ui,
                render_hud,
                false,
                render_no_post,
                render_balance,
                previewp->mSnapshotBufferType,
                previewp->getMaxImageSize()))
        {
            // Invalidate/delete any existing encoded image
            previewp->mPreviewImageEncoded = NULL;
            // Invalidate/delete any existing formatted image
            previewp->mFormattedImage = NULL;
            // Update the data size
            previewp->estimateDataSize();

            // Full size preview is set: get the decoded image result and save it for animation
            if (use_freeze_frame && previewp->mAllowFullScreenPreview)
            {
                previewp->prepareFreezeFrame();
            }

            // The snapshot is updated now...
            previewp->mSnapshotUpToDate = true;

            // We need to update the thumbnail though
            previewp->setThumbnailImageSize();
            previewp->generateThumbnailImage(true) ;
        }
        previewp->getWindow()->decBusyCount();
        previewp->setVisible(use_freeze_frame && previewp->mAllowFullScreenPreview); // only show fullscreen preview when in freeze frame mode
        previewp->mSnapshotActive = false;
        LL_DEBUGS("Snapshot") << "done creating snapshot" << LL_ENDL;
    }

    if (!previewp->getThumbnailUpToDate())
    {
        previewp->generateThumbnailImage() ;
    }

    // Tell the floater container that the snapshot is updated now
    if (previewp->mViewContainer)
    {
        previewp->mViewContainer->notify(LLSD().with("snapshot-updated", true));
    }

    return true;
}

void LLSnapshotLivePreview::prepareFreezeFrame()
{
    // Get the decoded version of the formatted image
    getEncodedImage();

    LLImageDataSharedLock lock(mPreviewImageEncoded);

    // We need to scale that a bit for display...
    LLPointer<LLImageRaw> scaled = new LLImageRaw(
        mPreviewImageEncoded->getData(),
        mPreviewImageEncoded->getWidth(),
        mPreviewImageEncoded->getHeight(),
        mPreviewImageEncoded->getComponents());

    if (!scaled->isBufferInvalid())
    {
        // leave original image dimensions, just scale up texture buffer
        if (mPreviewImageEncoded->getWidth() > 1024 || mPreviewImageEncoded->getHeight() > 1024)
        {
            // go ahead and shrink image to appropriate power of 2 for display
            scaled->biasedScaleToPowerOfTwo(1024);
            setImageScaled(true);
        }
        else
        {
            // expand image but keep original image data intact
            scaled->expandToPowerOfTwo(1024, false);
        }

        mViewerImage[mCurImageIndex] = LLViewerTextureManager::getLocalTexture(scaled.get(), false);
        LLPointer<LLViewerTexture> curr_preview_image = mViewerImage[mCurImageIndex];
        gGL.getTexUnit(0)->bind(curr_preview_image);
        curr_preview_image->setFilteringOption(getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE ? LLTexUnit::TFO_ANISOTROPIC : LLTexUnit::TFO_POINT);
        curr_preview_image->setAddressMode(LLTexUnit::TAM_CLAMP);


        if (gSavedSettings.getBOOL("UseFreezeFrame") && mAllowFullScreenPreview)
        {
            mShineCountdown = 4; // wait a few frames to avoid animation glitch due to readback this frame
        }
    }
}

S32 LLSnapshotLivePreview::getEncodedImageWidth() const
{
    S32 width = getWidth();
    if (getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE)
    {
        width = LLImageRaw::biasedDimToPowerOfTwo(width,MAX_TEXTURE_SIZE);
    }
    return width;
}
S32 LLSnapshotLivePreview::getEncodedImageHeight() const
{
    S32 height = getHeight();
    if (getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE)
    {
        height = LLImageRaw::biasedDimToPowerOfTwo(height,MAX_TEXTURE_SIZE);
    }
    return height;
}

LLPointer<LLImageRaw> LLSnapshotLivePreview::getEncodedImage()
{
    if (!mPreviewImageEncoded)
    {
        LLImageDataSharedLock lock(mPreviewImage);

        mPreviewImageEncoded = new LLImageRaw;

        mPreviewImageEncoded->resize(
            mPreviewImage->getWidth(),
            mPreviewImage->getHeight(),
            mPreviewImage->getComponents());

        if (getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE)
        {
            // We don't store the intermediate formatted image in mFormattedImage in the J2C case
            LL_DEBUGS("Snapshot") << "Encoding new image of format J2C" << LL_ENDL;
            LLPointer<LLImageJ2C> formatted = new LLImageJ2C;
            // Copy the preview
            LLPointer<LLImageRaw> scaled = new LLImageRaw(
                                                          mPreviewImage->getData(),
                                                          mPreviewImage->getWidth(),
                                                          mPreviewImage->getHeight(),
                                                          mPreviewImage->getComponents());
            // Scale it as required by J2C
            scaled->biasedScaleToPowerOfTwo(MAX_TEXTURE_SIZE);
            setImageScaled(true);
            // Compress to J2C
            if (formatted->encode(scaled, 0.f))
            {
                // We can update the data size precisely at that point
                mDataSize = formatted->getDataSize();
                // Decompress back
                formatted->decode(mPreviewImageEncoded, 0);
            }
        }
        else
        {
            // Update mFormattedImage if necessary
            lock.unlock();
            getFormattedImage(); // will apply filters to mPreviewImage with a lock
            lock.lock();
            if (getSnapshotFormat() == LLSnapshotModel::SNAPSHOT_FORMAT_BMP)
            {
                // BMP hack : copy instead of decode otherwise decode will crash.
                mPreviewImageEncoded->copy(mPreviewImage);
            }
            else
            {
                // Decode back
                mFormattedImage->decode(mPreviewImageEncoded, 0);
            }
        }
    }
    return mPreviewImageEncoded;
}

bool LLSnapshotLivePreview::createUploadFile(const std::string &out_filename, const S32 max_image_dimentions, const S32 min_image_dimentions)
{
    return LLViewerTextureList::createUploadFile(mPreviewImage, out_filename, max_image_dimentions, min_image_dimentions);
}

// We actually estimate the data size so that we do not require actual compression when showing the preview
// Note : whenever formatted image is computed, mDataSize will be updated to reflect the true size
void LLSnapshotLivePreview::estimateDataSize()
{
    // Compression ratio
    F32 ratio = 1.0;

    if (getSnapshotType() == LLSnapshotModel::SNAPSHOT_TEXTURE)
    {
        ratio = 8.0;    // This is what we shoot for when compressing to J2C
    }
    else
    {
        LLSnapshotModel::ESnapshotFormat format = getSnapshotFormat();
        switch (format)
        {
            case LLSnapshotModel::SNAPSHOT_FORMAT_PNG:
                ratio = 3.0;    // Average observed PNG compression ratio
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_JPEG:
                // Observed from JPG compression tests
                ratio = (F32)(110 - mSnapshotQuality) / 2.f;
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_BMP:
                ratio = 1.0;    // No compression with BMP
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_WEBP:
                ratio = 4.0;    // Average observed WebP compression ratio
                break;
        }
    }
    mDataSize = (S32)((F32)mPreviewImage->getDataSize() / ratio);
}

LLPointer<LLImageFormatted> LLSnapshotLivePreview::getFormattedImage()
{
    if (!mFormattedImage)
    {
        // Apply the filter to mPreviewImage
        if (getFilter() != "")
        {
            std::string filter_path = LLImageFiltersManager::getInstance()->getFilterPath(getFilter());
            if (filter_path != "")
            {
                LLImageFilter filter(filter_path);
                filter.executeFilter(mPreviewImage);
            }
            else
            {
                LL_WARNS("Snapshot") << "Couldn't find a path to the following filter : " << getFilter() << LL_ENDL;
            }
        }

        // Create the new formatted image of the appropriate format.
        LLSnapshotModel::ESnapshotFormat format = getSnapshotFormat();
        LL_DEBUGS("Snapshot") << "Encoding new image of format " << format << LL_ENDL;

        switch (format)
        {
            case LLSnapshotModel::SNAPSHOT_FORMAT_PNG:
                mFormattedImage = new LLImagePNG();
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_JPEG:
                mFormattedImage = new LLImageJPEG(mSnapshotQuality);
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_BMP:
                mFormattedImage = new LLImageBMP();
                break;
            case LLSnapshotModel::SNAPSHOT_FORMAT_WEBP:
                mFormattedImage = new LLImageWebP();
                break;
        }
        if (mFormattedImage->encode(mPreviewImage, 0))
        {
            // We can update the data size precisely at that point
            mDataSize = mFormattedImage->getDataSize();
        }
    }
    return mFormattedImage;
}

void LLSnapshotLivePreview::setSize(S32 w, S32 h)
{
    LL_DEBUGS("Snapshot") << "setSize(" << w << ", " << h << ")" << LL_ENDL;
    setWidth(w);
    setHeight(h);
}

void LLSnapshotLivePreview::setSnapshotFormat(LLSnapshotModel::ESnapshotFormat format)
{
    if (mSnapshotFormat != format)
    {
        mFormattedImage = NULL;     // Invalidate the already formatted image if any
        mSnapshotFormat = format;
    }
}

void LLSnapshotLivePreview::getSize(S32& w, S32& h) const
{
    w = getWidth();
    h = getHeight();
}

void LLSnapshotLivePreview::saveTexture(bool outfit_snapshot, std::string name)
{
    LLImageDataSharedLock lock(mPreviewImage);

    LL_DEBUGS("Snapshot") << "saving texture: " << mPreviewImage->getWidth() << "x" << mPreviewImage->getHeight() << LL_ENDL;
    // gen a new uuid for this asset
    LLTransactionID tid;
    tid.generate();
    LLAssetID new_asset_id = tid.makeAssetID(gAgent.getSecureSessionID());

    LLPointer<LLImageJ2C> formatted = new LLImageJ2C;

    // <AS:Chanayane> Lossless OpenJPEG uploads
    formatted->setReversible(true);
    // </AS:Chanayane>

    LLPointer<LLImageRaw> scaled = new LLImageRaw(mPreviewImage->getData(),
        mPreviewImage->getWidth(),
        mPreviewImage->getHeight(),
        mPreviewImage->getComponents());

    // Apply the filter to mPreviewImage
    if (getFilter() != "")
    {
        std::string filter_path = LLImageFiltersManager::getInstance()->getFilterPath(getFilter());
        if (filter_path != "")
        {
            LLImageFilter filter(filter_path);
            filter.executeFilter(scaled);
        }
        else
        {
            LL_WARNS("Snapshot") << "Couldn't find a path to the following filter : " << getFilter() << LL_ENDL;
        }
    }

    scaled->biasedScaleToPowerOfTwo(MAX_TEXTURE_SIZE);
    LL_DEBUGS("Snapshot") << "scaled texture to " << scaled->getWidth() << "x" << scaled->getHeight() << LL_ENDL;

    if (formatted->encode(scaled, 0.0f))
    {
        LLFileSystem fmt_file(new_asset_id, LLAssetType::AT_TEXTURE, LLFileSystem::WRITE);
        fmt_file.write(formatted->getData(), formatted->getDataSize());
        std::string pos_string;
        LLAgentUI::buildLocationString(pos_string, LLAgentUI::LOCATION_FORMAT_FULL);
        std::string who_took_it;
        LLAgentUI::buildFullname(who_took_it);
        S32 expected_upload_cost = LLAgentBenefitsMgr::current().getTextureUploadCost(scaled->getWidth(), scaled->getHeight());
        std::string res_name = outfit_snapshot ? name : "Snapshot : " + pos_string;
        std::string res_desc = outfit_snapshot ? "" : "Taken by " + who_took_it + " at " + pos_string;
        LLFolderType::EType folder_type = outfit_snapshot ? LLFolderType::FT_NONE : LLFolderType::FT_SNAPSHOT_CATEGORY;
        LLInventoryType::EType inv_type = outfit_snapshot ? LLInventoryType::IT_NONE : LLInventoryType::IT_SNAPSHOT;

        LLResourceUploadInfo::ptr_t assetUploadInfo(new LLResourceUploadInfo(
            tid, LLAssetType::AT_TEXTURE, res_name, res_desc, 0,
            folder_type, inv_type,
            PERM_ALL, LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getEveryonePerms("Uploads"),
            expected_upload_cost, !outfit_snapshot));

        upload_new_resource(assetUploadInfo);

        gViewerWindow->playSnapshotAnimAndSound();
    }
    else
    {
        LLNotificationsUtil::add("ErrorEncodingSnapshot");
        LL_WARNS("Snapshot") << "Error encoding snapshot" << LL_ENDL;
    }

    add(LLStatViewer::SNAPSHOT, 1);

    mDataSize = 0;
}

void LLSnapshotLivePreview::saveLocal(const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    // Update mFormattedImage if necessary
    getFormattedImage();

    // Save the formatted image
    saveLocal(mFormattedImage, success_cb, failure_cb);
}

//Check if failed due to insufficient memory
void LLSnapshotLivePreview::saveLocal(LLPointer<LLImageFormatted> image, const snapshot_saved_signal_t::slot_type& success_cb, const snapshot_saved_signal_t::slot_type& failure_cb)
{
    sSaveLocalImage = image;

    gViewerWindow->saveImageNumbered(sSaveLocalImage, false, success_cb, failure_cb);
}
