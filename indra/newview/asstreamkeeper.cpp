/**
 * @file asstreamkeeper.cpp
 * @brief AyaneStorm audio stream keeper and favorite streams
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (c) 2026 Chanayane @ Second Life
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "asstreamkeeper.h"

#include "asfloaterfavoritestreams.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llpanel.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "llstreamingaudio.h"
#include "llvieweraudio.h"
#include "llviewercontrol.h"
#include "llviewerparcelmedia.h"
#include "llviewerparcelmgr.h"

static const std::string AS_FAVORITE_STREAMS_FILE("as_favorite_streams.xml");

// How long a held stream may report "stopped" before we assume it is dead and
// release the hold. Without this a dead stream would block parcel music forever.
static const F32 AS_DEAD_STREAM_TIMEOUT = 10.0f;

// ----------------------------------------------------------------------------
// ASFavoriteStream
// ----------------------------------------------------------------------------

LLSD ASFavoriteStream::toLLSD() const
{
    LLSD data;
    data["name"] = mName;
    data["url"] = mURL;
    return data;
}

// static
ASFavoriteStream ASFavoriteStream::fromLLSD(const LLSD& data)
{
    ASFavoriteStream fav;
    if (data.isMap())
    {
        fav.mName = data["name"].asString();
        fav.mURL = data["url"].asString();
    }
    return fav;
}

// ----------------------------------------------------------------------------
// ASStreamKeeper
// ----------------------------------------------------------------------------

ASStreamKeeper::ASStreamKeeper()
    : mHoldReason(HOLD_NONE),
      mWatchdogActive(false)
{
}

ASStreamKeeper::~ASStreamKeeper()
{
    stopWatchdog();
}

// static
bool ASStreamKeeper::allowParcelStreamChange()
{
    // Checking instanceExists() first means nothing is ever constructed when
    // the feature is unused.
    return !instanceExists() || getInstance()->mHoldReason == HOLD_NONE;
}

// static
bool ASStreamKeeper::showAddFavoriteStreamButton()
{
    static LLCachedControl<bool> show_button(gSavedSettings, "ASShowAddFavoriteStreamButton", true);
    return show_button;
}

// static
boost::signals2::connection ASStreamKeeper::setAddFavoriteStreamButtonChangedCallback(
    const change_signal_t::slot_type& cb)
{
    return gSavedSettings.getControl("ASShowAddFavoriteStreamButton")->getSignal()->connect(
        boost::bind(cb));
}

// static
void ASStreamKeeper::updateAddFavoriteStreamButtonLayout(LLPanel* status_bar,
                                                         bool stream_toggle_visible)
{
    if (!status_bar)
    {
        return;
    }

    LLButton* add_btn = status_bar->findChild<LLButton>("as_add_favorite_stream_btn");
    if (!add_btn)
    {
        return;
    }

    const bool show = showAddFavoriteStreamButton();
    add_btn->setVisible(stream_toggle_visible && show);

    // The button sits inside the right-anchored "time_and_media_bg" panel,
    // whose children have fixed positions (XUI resolves left_pad at build
    // time), so hiding it leaves a hole. media_toggle_btn/volume_btn/FPSText
    // sit to its right and must stay put - the "time_and_media_bg" panel
    // itself is anchored to the window's right edge via those trailing
    // widgets, so shifting them would detach the panel from that edge and
    // clip the bar. Close the hole instead by shifting the widgets to its
    // left - stream_toggle_btn - right when hidden, left when shown.
    //
    // Keyed off the setting alone, not the button's visibility: when the volume
    // controls are hidden, updateVolumeControlsVisibility() already reclaims the
    // whole icon span including this button, and shifting again would double up.
    //
    // The applied offset is remembered so repeated calls cannot accumulate.
    static S32 applied_offset = 0;

    const S32 wanted_offset = (show ? 0 : add_btn->getRect().getWidth() + 2);
    const S32 delta = wanted_offset - applied_offset;
    if (delta == 0)
    {
        return;
    }
    applied_offset = wanted_offset;

    // Positive: reclaiming width moves the widgets to its left rightwards.
    const S32 shift = delta;

    static const char* AS_SIBLINGS_TO_SHIFT[] = {
        "stream_toggle_btn"
    };

    for (const char* name : AS_SIBLINGS_TO_SHIFT)
    {
        if (LLView* view = status_bar->findChild<LLView>(name))
        {
            view->translate(shift, 0);
        }
    }
}

// static
bool ASStreamKeeper::suppressTeleportStreamFade()
{
    if (!instanceExists())
    {
        return false;
    }

    // Already holding: allowParcelStreamChange() covers this, but checking here
    // keeps the two callers independent.
    if (getInstance()->mHoldReason != HOLD_NONE)
    {
        return true;
    }

    static LLCachedControl<bool> ask_keep(gSavedSettings, "ASKeepStreamAsk", false);
    if (!ask_keep || !gAudiop)
    {
        return false;
    }

    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return false;
    }

    // Only hold the stream through the teleport when there is genuinely
    // something playing worth keeping - otherwise let the fade run as vanilla.
    return !gAudiop->getInternetStreamURL().empty()
           && LLAudioEngine::AUDIO_PLAYING == gAudiop->isInternetStreamPlaying();
}

// static
bool ASStreamKeeper::handleParcelStreamChange(const std::string& parcel_url)
{
    // The singleton already exists after login (it owns the favorites list), so
    // the ordering here matters: bail out on the cheap checks before doing any
    // work, and only then consider acting.
    if (!instanceExists())
    {
        return false;
    }

    ASStreamKeeper* self = getInstance();

    // Already holding or prompting: never re-prompt and never stack prompts.
    // This is also what lets a playing favorite continue silently across
    // parcel changes and teleports.
    if (self->mHoldReason != HOLD_NONE)
    {
        return true;
    }

    static LLCachedControl<bool> ask_keep(gSavedSettings, "ASKeepStreamAsk", false);
    static LLCachedControl<bool> autoplay_fav(gSavedSettings, "ASAutoPlayDefaultFavorite", false);

    // Both features off: behave exactly as an unpatched viewer.
    if (!ask_keep && !autoplay_fav)
    {
        return false;
    }

    // Never act before the viewer has finished logging in, otherwise the
    // prompt turns into a "new notifications arrived" toast.
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return false;
    }

    // Play the chosen default favorite instead of the parcel stream.
    if (autoplay_fav)
    {
        S32 idx = self->getDefaultFavoriteIndex();
        if (idx >= 0)
        {
            self->playFavorite((size_t)idx);
            return true;
        }
        // Unset or stale default: fall through to vanilla behavior.
    }

    if (!ask_keep || !gAudiop)
    {
        return false;
    }

    // What is actually playing, not LLViewerAudio::getNextStreamURI(), which
    // during a fade is the pending target and would give false positives.
    const std::string current_url = gAudiop->getInternetStreamURL();
    if (current_url.empty() || LLAudioEngine::AUDIO_PLAYING != gAudiop->isInternetStreamPlaying())
    {
        return false;   // nothing playing that is worth keeping
    }

    if (!parcel_url.empty() && parcel_url == current_url)
    {
        return false;   // same stream, nothing to ask about
    }

    self->cancelPrompt();

    LLSD args;
    args["CURRENT_URL"] = current_url;
    args["PARCEL_URL"] = parcel_url;

    LLSD payload;
    payload["current_url"] = current_url;
    payload["parcel_url"] = parcel_url;

    // Two variants because notification form button labels are static.
    const std::string notification_name = parcel_url.empty() ? "ASKeepCurrentStreamNoParcel"
                                                             : "ASKeepCurrentStream";

    // HOLD_PENDING blocks every other clobber path (teleport handlers, the
    // media proximity path) while the prompt is open, so the stream keeps
    // playing untouched until the user answers.
    self->mHoldReason = HOLD_PENDING;
    self->mHeldStreamURL = current_url;
    self->mPrompt = LLNotificationsUtil::add(notification_name, args, payload,
                                             &ASStreamKeeper::onPromptResponse);

    LL_INFOS("StreamKeeper") << "Asking whether to keep current stream " << current_url
                             << " (parcel stream: '" << parcel_url << "')" << LL_ENDL;

    // The caller must not stop or switch anything.
    return true;
}

// static
bool ASStreamKeeper::onPromptResponse(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    const std::string current_url = notification["payload"]["current_url"].asString();
    const std::string parcel_url = notification["payload"]["parcel_url"].asString();

    ASStreamKeeper* self = getInstance();
    self->mPrompt = NULL;

    if (option == 0)
    {
        // Keep current. The audio was never touched, so there is nothing to
        // restart - it simply keeps playing.
        self->mHoldReason = HOLD_KEPT;
        self->mHeldStreamURL = current_url;
        self->syncStatusBar(true);
        self->startWatchdog();

        LL_INFOS("StreamKeeper") << "Keeping current stream " << current_url << LL_ENDL;
    }
    else
    {
        // Switch to the parcel stream, or stop when the parcel has none.
        self->mHoldReason = HOLD_NONE;
        self->mHeldStreamURL.clear();
        self->stopWatchdog();

        if (parcel_url.empty())
        {
            LL_INFOS("StreamKeeper") << "Stopping stream at user request" << LL_ENDL;
            LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
            self->syncStatusBar(false);
        }
        else
        {
            LL_INFOS("StreamKeeper") << "Switching to parcel stream " << parcel_url << LL_ENDL;

            // Choosing "switch" is the user asking for streams, so record that
            // intent up front: it is what stops the next parcel change from
            // silencing us, and it does not depend on the filter's answer.
            //
            // Nothing else here asserts that audio is playing. With
            // MediaEnableFilter on, an unknown domain only raises the filter
            // prompt - playback starts when the user allows it, and denying
            // stops the stream instead. The toggle's playing/stopped state is
            // read from the real audio engine every frame in
            // LLStatusBar::refresh(), so it settles correctly either way.
            self->syncStatusBar(true);
            startStreamFiltered(parcel_url);
        }
    }

    return false;
}

void ASStreamKeeper::cancelPrompt()
{
    if (mPrompt)
    {
        if (!mPrompt->isCancelled())
        {
            // Force a response. Mirrors LLViewerParcelAskPlay::cancelNotification().
            mPrompt->setIgnored(false);
            LLNotifications::getInstance()->cancel(mPrompt);
        }
        mPrompt = NULL;
    }

    // A dismissed prompt must never leave a permanent hold behind.
    if (mHoldReason == HOLD_PENDING)
    {
        mHoldReason = HOLD_NONE;
        mHeldStreamURL.clear();
    }
}

// ----------------------------------------------------------------------------
// Playback
// ----------------------------------------------------------------------------

// static
void ASStreamKeeper::startStreamFiltered(const std::string& url)
{
    if (gSavedSettings.getBOOL("MediaEnableFilter"))
    {
        LLViewerParcelMedia* media = LLViewerParcelMedia::getInstance();

        // filterAudioUrl() has two fast paths that start playback with no
        // prompt: url == mCurrentMusic, and url == mAudioLastURL when the last
        // action was "play". Both caches are only refreshed when the filter
        // actually runs - and raising the keep prompt suppresses the vanilla
        // parcel handling that would have run it. So they can still hold a
        // verdict from an earlier parcel, and hitting either one here would
        // play a stream the user was never asked about in this context.
        //
        // Clearing them forces a real evaluation: whitelisted domains still
        // play silently, blacklisted ones are still blocked, and anything
        // unknown prompts as it should.
        media->mCurrentMusic.clear();
        media->mAudioLastURL.clear();

        media->filterAudioUrl(url);
    }
    else
    {
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);
    }
}

void ASStreamKeeper::playStreamURL(const std::string& url, EHoldReason reason)
{
    if (url.empty())
    {
        return;
    }

    cancelPrompt();

    mHoldReason = reason;
    mHeldStreamURL = url;

    // Favorites are chosen by the user and therefore trusted, so they bypass
    // the media filter - that filter exists to vet parcel-supplied URLs, and
    // running it here would prompt for the user's own saved station every
    // time. This matches how the /music <url> command behaves.
    LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);

    syncStatusBar(true);
    startWatchdog();

    LL_INFOS("StreamKeeper") << "Playing stream " << url << LL_ENDL;
}

void ASStreamKeeper::playFavorite(size_t index)
{
    if (index >= mFavorites.size())
    {
        return;
    }
    playStreamURL(mFavorites[index].mURL, HOLD_FAVORITE);
}

void ASStreamKeeper::playParcelStream()
{
    cancelPrompt();

    // Release the hold before playing so that normal parcel logic resumes from
    // here on.
    mHoldReason = HOLD_NONE;
    mHeldStreamURL.clear();
    stopWatchdog();

    const std::string url = getAgentParcelMusicURL();
    if (url.empty())
    {
        LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
        syncStatusBar(false);
    }
    else
    {
        startStreamFiltered(url);
        syncStatusBar(true);
    }

    LL_INFOS("StreamKeeper") << "Returning to parcel stream '" << url << "'" << LL_ENDL;
}

void ASStreamKeeper::releaseHold()
{
    cancelPrompt();
    mHoldReason = HOLD_NONE;
    mHeldStreamURL.clear();
    stopWatchdog();
}

void ASStreamKeeper::syncStatusBar(bool stream_enabled)
{
    // LLStatusBar::mAudioStreamEnabled is read by the parcel code as the
    // "user wants streams" signal, so anything that starts audio outside the
    // status bar has to keep it in sync or the next parcel change stops us.
    if (gStatusBar)
    {
        gStatusBar->setAudioStreamEnabled(stream_enabled);
    }
}

// static
std::string ASStreamKeeper::getAgentParcelMusicURL()
{
    std::string url;

    // Note: deliberately not LLViewerMedia::getParcelAudioURL(), which
    // dereferences getAgentParcel() without a null check.
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (parcel)
    {
        url = parcel->getMusicURL();
        LLStringUtil::trim(url);
    }
    return url;
}

std::string ASStreamKeeper::getCurrentPlayingURL() const
{
    if (!gAudiop)
    {
        return LLStringUtil::null;
    }
    return gAudiop->getInternetStreamURL();
}

// ----------------------------------------------------------------------------
// Dead stream watchdog
// ----------------------------------------------------------------------------

void ASStreamKeeper::startWatchdog()
{
    mDeadStreamTimer.reset();

    if (!mWatchdogActive)
    {
        mWatchdogActive = true;
        doOnIdleRepeating(boost::bind(&ASStreamKeeper::onWatchdogTick, this));
    }
}

void ASStreamKeeper::stopWatchdog()
{
    // onWatchdogTick() deregisters itself once it sees this cleared.
    mWatchdogActive = false;
}

bool ASStreamKeeper::onWatchdogTick()
{
    if (!mWatchdogActive)
    {
        return true;    // done, remove the callback
    }

    if (mHoldReason == HOLD_NONE)
    {
        mWatchdogActive = false;
        return true;
    }

    // A prompt being open is not a stream failure.
    if (mHoldReason == HOLD_PENDING)
    {
        mDeadStreamTimer.reset();
        return false;
    }

    if (gAudiop && LLAudioEngine::AUDIO_STOPPED == gAudiop->isInternetStreamPlaying())
    {
        if (mDeadStreamTimer.getElapsedTimeF32() > AS_DEAD_STREAM_TIMEOUT)
        {
            LL_INFOS("StreamKeeper") << "Held stream " << mHeldStreamURL
                                     << " appears dead; releasing hold" << LL_ENDL;
            releaseHold();
            return true;
        }
    }
    else
    {
        mDeadStreamTimer.reset();
    }

    return false;
}

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

// static
void ASStreamKeeper::onLoginComplete()
{
    getInstance()->loadFavorites();
}

// static
void ASStreamKeeper::onLogout()
{
    if (!instanceExists())
    {
        return;
    }

    ASStreamKeeper* self = getInstance();
    self->releaseHold();
    self->mFavorites.clear();
    // Must clear the filename too, otherwise the next account's session could
    // save this account's (now empty) list over the wrong file.
    self->mFavoritesFilename.clear();
    self->mFavoritesChangedSignal();
}

// static
void ASStreamKeeper::releaseHoldIfAny()
{
    if (instanceExists())
    {
        getInstance()->releaseHold();
    }
}

// static
void ASStreamKeeper::onTeleportStarted()
{
    if (instanceExists())
    {
        getInstance()->cancelPrompt();
    }
}

// static
void ASStreamKeeper::onShutdown()
{
    if (instanceExists())
    {
        getInstance()->saveFavorites();
    }
}

// ----------------------------------------------------------------------------
// Favorites
// ----------------------------------------------------------------------------

std::string ASStreamKeeper::getFavoritesFilename() const
{
    // Before login LL_PATH_PER_SL_ACCOUNT resolves to an empty directory, and
    // getExpandedFilename() would then hand back a bare filename that lands in
    // the working directory. Probe for the directory first.
    std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "");
    if (!path.empty())
    {
        path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, AS_FAVORITE_STREAMS_FILE);
    }
    return path;
}

void ASStreamKeeper::loadFavorites()
{
    mFavorites.clear();
    mFavoritesFilename = getFavoritesFilename();

    if (mFavoritesFilename.empty())
    {
        LL_WARNS("StreamKeeper") << "No per-account directory; favorite streams unavailable" << LL_ENDL;
        return;
    }

    if (!gDirUtilp->fileExists(mFavoritesFilename))
    {
        mFavoritesChangedSignal();
        return;
    }

    llifstream stream(mFavoritesFilename.c_str());
    if (!stream.is_open())
    {
        LL_WARNS("StreamKeeper") << "Could not open " << mFavoritesFilename << LL_ENDL;
        return;
    }

    LLSD data;
    if (LLSDSerialize::fromXML(data, stream) >= 1 && data.isArray())
    {
        for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
        {
            ASFavoriteStream fav = ASFavoriteStream::fromLLSD(*it);
            if (!fav.mURL.empty())
            {
                mFavorites.push_back(fav);
            }
        }
    }
    else
    {
        LL_WARNS("StreamKeeper") << "Could not parse " << mFavoritesFilename << LL_ENDL;
    }
    stream.close();

    LL_INFOS("StreamKeeper") << "Loaded " << mFavorites.size() << " favorite stream(s)" << LL_ENDL;

    mFavoritesChangedSignal();
}

void ASStreamKeeper::saveFavorites() const
{
    if (mFavoritesFilename.empty())
    {
        return;
    }

    LLSD data = LLSD::emptyArray();
    for (favorites_vec_t::const_iterator it = mFavorites.begin(); it != mFavorites.end(); ++it)
    {
        data.append(it->toLLSD());
    }

    llofstream file;
    file.open(mFavoritesFilename.c_str());
    if (!file.is_open())
    {
        LL_WARNS("StreamKeeper") << "Could not write " << mFavoritesFilename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
    file.close();
}

void ASStreamKeeper::onFavoritesChanged()
{
    // Save eagerly so the list survives a crash, then let the floater refresh.
    saveFavorites();
    mFavoritesChangedSignal();
}

bool ASStreamKeeper::hasFavoriteURL(const std::string& url) const
{
    for (favorites_vec_t::const_iterator it = mFavorites.begin(); it != mFavorites.end(); ++it)
    {
        if (it->mURL == url)
        {
            return true;
        }
    }
    return false;
}

bool ASStreamKeeper::addFavorite(const std::string& name, const std::string& url)
{
    std::string trimmed_url(url);
    LLStringUtil::trim(trimmed_url);
    if (trimmed_url.empty() || hasFavoriteURL(trimmed_url))
    {
        return false;
    }

    std::string trimmed_name(name);
    LLStringUtil::trim(trimmed_name);
    if (trimmed_name.empty())
    {
        trimmed_name = trimmed_url;
    }

    mFavorites.push_back(ASFavoriteStream(trimmed_name, trimmed_url));
    onFavoritesChanged();
    return true;
}

bool ASStreamKeeper::editFavorite(size_t index, const std::string& name, const std::string& url)
{
    if (index >= mFavorites.size())
    {
        return false;
    }

    std::string trimmed_url(url);
    LLStringUtil::trim(trimmed_url);
    if (trimmed_url.empty())
    {
        return false;
    }

    // Allow the entry to keep its own URL, but not to collide with another.
    for (size_t i = 0; i < mFavorites.size(); ++i)
    {
        if (i != index && mFavorites[i].mURL == trimmed_url)
        {
            return false;
        }
    }

    std::string trimmed_name(name);
    LLStringUtil::trim(trimmed_name);
    if (trimmed_name.empty())
    {
        trimmed_name = trimmed_url;
    }

    // Keep the default pointing at this entry if its URL changed.
    const std::string old_url = mFavorites[index].mURL;
    if (old_url != trimmed_url
        && gSavedPerAccountSettings.getString("ASDefaultFavoriteStreamURL") == old_url)
    {
        gSavedPerAccountSettings.setString("ASDefaultFavoriteStreamURL", trimmed_url);
    }

    mFavorites[index].mName = trimmed_name;
    mFavorites[index].mURL = trimmed_url;
    onFavoritesChanged();
    return true;
}

bool ASStreamKeeper::removeFavorite(size_t index)
{
    if (index >= mFavorites.size())
    {
        return false;
    }

    if (gSavedPerAccountSettings.getString("ASDefaultFavoriteStreamURL") == mFavorites[index].mURL)
    {
        gSavedPerAccountSettings.setString("ASDefaultFavoriteStreamURL", LLStringUtil::null);
    }

    mFavorites.erase(mFavorites.begin() + index);
    onFavoritesChanged();
    return true;
}

bool ASStreamKeeper::removeFavoriteByURL(const std::string& url)
{
    for (size_t i = 0; i < mFavorites.size(); ++i)
    {
        if (mFavorites[i].mURL == url)
        {
            return removeFavorite(i);
        }
    }
    return false;
}

bool ASStreamKeeper::moveFavorite(size_t from, size_t to)
{
    if (from >= mFavorites.size() || to >= mFavorites.size() || from == to)
    {
        return false;
    }

    ASFavoriteStream fav = mFavorites[from];
    mFavorites.erase(mFavorites.begin() + from);
    mFavorites.insert(mFavorites.begin() + to, fav);
    onFavoritesChanged();
    return true;
}

S32 ASStreamKeeper::getDefaultFavoriteIndex() const
{
    const std::string url = gSavedPerAccountSettings.getString("ASDefaultFavoriteStreamURL");
    if (url.empty())
    {
        return -1;
    }

    for (size_t i = 0; i < mFavorites.size(); ++i)
    {
        if (mFavorites[i].mURL == url)
        {
            return (S32)i;
        }
    }

    // Stale: the favorite was removed. Callers fall back to vanilla behavior.
    return -1;
}

void ASStreamKeeper::setDefaultFavorite(size_t index)
{
    if (index >= mFavorites.size())
    {
        return;
    }
    gSavedPerAccountSettings.setString("ASDefaultFavoriteStreamURL", mFavorites[index].mURL);
    mFavoritesChangedSignal();
}

void ASStreamKeeper::clearDefaultFavorite()
{
    gSavedPerAccountSettings.setString("ASDefaultFavoriteStreamURL", LLStringUtil::null);
    mFavoritesChangedSignal();
}

std::string ASStreamKeeper::getSuggestedNameForCurrentStream() const
{
    const std::string url = getCurrentPlayingURL();
    if (url.empty())
    {
        return LLStringUtil::null;
    }

    // Use the host portion of the URL: it identifies the station and stays
    // meaningful, unlike the currently playing track from the ICY metadata.
    std::string host = url;
    size_t scheme = host.find("://");
    if (scheme != std::string::npos)
    {
        host = host.substr(scheme + 3);
    }
    size_t slash = host.find('/');
    if (slash != std::string::npos)
    {
        host = host.substr(0, slash);
    }

    return host.empty() ? url : host;
}

// static
bool ASStreamKeeper::promptAddCurrentStream()
{
    ASStreamKeeper* self = getInstance();

    const std::string url = self->getCurrentPlayingURL();
    if (url.empty() || self->hasFavoriteURL(url))
    {
        // Explains why nothing happened; kept here so callers stay one-liners.
        LLNotificationsUtil::add("ASFavoriteStreamNotAdded");
        return false;
    }

    // Show first: this creates the floater if it has never been opened, so the
    // typed lookup below always succeeds.
    LLFloaterReg::showInstance("as_favoritestreams");

    ASFloaterFavoriteStreams* floater =
        LLFloaterReg::getTypedInstance<ASFloaterFavoriteStreams>("as_favoritestreams");
    if (!floater)
    {
        return false;
    }

    floater->beginAddStream(self->getSuggestedNameForCurrentStream(), url);
    return true;
}
