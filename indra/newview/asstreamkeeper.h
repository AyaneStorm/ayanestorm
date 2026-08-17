/**
 * @file asstreamkeeper.h
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

#ifndef AS_STREAMKEEPER_H
#define AS_STREAMKEEPER_H

#include "llframetimer.h"
#include "llnotificationptr.h"
#include "llsingleton.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

class LLPanel;

// A single saved audio stream entry.
class ASFavoriteStream
{
public:
    ASFavoriteStream() = default;
    ASFavoriteStream(const std::string& name, const std::string& url)
        : mName(name), mURL(url) {}

    std::string mName;
    std::string mURL;

    LLSD toLLSD() const;
    static ASFavoriteStream fromLLSD(const LLSD& data);
};

// Owns all of the AyaneStorm stream-keeper state:
//  - the "hold", which prevents parcel changes from clobbering a stream the
//    user deliberately chose,
//  - the keep-current-stream prompt,
//  - the per-account favorite stream list.
//
// Firestorm files only ever call the statics in the "Hooks" section below; all
// decisions live here so the feature can be extracted cleanly.
class ASStreamKeeper : public LLSingleton<ASStreamKeeper>
{
    LLSINGLETON(ASStreamKeeper);
    ~ASStreamKeeper();

public:
    enum EHoldReason
    {
        HOLD_NONE,      // not holding; parcel behaves exactly as vanilla
        HOLD_PENDING,   // prompt is open; keep playing and block parcel changes
        HOLD_KEPT,      // user answered "Keep current"
        HOLD_FAVORITE   // user is playing a favorite stream
    };

    typedef std::vector<ASFavoriteStream>   favorites_vec_t;
    typedef boost::signals2::signal<void()> change_signal_t;

    // ---- Hooks called from Firestorm files ----------------------------------

    // true  => the parcel/media subsystem may proceed exactly as vanilla.
    // false => a stream is being held; the caller must skip its normal action.
    static bool allowParcelStreamChange();

    // Called from the parcel music paths. An empty parcel_url means the new
    // parcel has no usable stream.
    // Returns true when the keeper has handled the change (prompt raised, or a
    // default favorite started), in which case the caller must not touch the
    // stream at all - that is what keeps the current stream playing.
    static bool handleParcelStreamChange(const std::string& parcel_url);

    static void onLoginComplete();
    static void onTeleportStarted();

    // true => the teleport handlers must leave the stream alone.
    //
    // A teleport starts before the destination parcel is known, so the keep
    // prompt cannot have been raised yet. Fading here would kill the very
    // stream the prompt is about to offer to keep, so the fade is suppressed
    // whenever a prompt is possible on arrival; if the user then answers
    // "switch", the parcel stream starts at that point instead.
    static bool suppressTeleportStreamFade();
    static void onShutdown();

    // Safe to call at any time, including before login: no-ops when no stream
    // is being held. Use this from Firestorm hooks rather than getInstance().
    static void releaseHoldIfAny();

    // Drops all per-account state. This viewer exits the process on logout, so
    // there is no call site today; it exists so that adding an in-process
    // relogin path cannot leak one account's favorites into another's file.
    static void onLogout();

    // ---- Playback -----------------------------------------------------------

    void playFavorite(size_t index);
    void playStreamURL(const std::string& url, EHoldReason reason);
    void playParcelStream();
    void releaseHold();

    // ---- Hold state ---------------------------------------------------------

    bool               isHoldingStream() const   { return mHoldReason != HOLD_NONE; }
    EHoldReason        getHoldReason() const     { return mHoldReason; }
    const std::string& getHeldStreamURL() const  { return mHeldStreamURL; }

    // ---- Favorites ----------------------------------------------------------

    const favorites_vec_t& getFavorites() const { return mFavorites; }

    bool addFavorite(const std::string& name, const std::string& url);
    bool editFavorite(size_t index, const std::string& name, const std::string& url);
    bool removeFavorite(size_t index);
    // Prefer this for deferred callbacks: indices go stale if the list changes
    // between raising a confirmation and the user answering it.
    bool removeFavoriteByURL(const std::string& url);
    bool moveFavorite(size_t from, size_t to);
    bool hasFavoriteURL(const std::string& url) const;

    // The default favorite is stored by URL, not by index, so that reordering
    // and removing entries cannot silently repoint it.
    S32  getDefaultFavoriteIndex() const;
    void setDefaultFavorite(size_t index);
    void clearDefaultFavorite();

    std::string getCurrentPlayingURL() const;
    // A starting-point name for the playing stream, derived from its URL host.
    // Deliberately not the ICY track metadata: that is the song playing right
    // now, not the station, and would bake a stale track name into the entry.
    std::string getSuggestedNameForCurrentStream() const;

    // Opens the favorites floater with the edit panel prefilled from whatever
    // is playing, so the user can name it before saving. Returns false when no
    // stream is playing or it is already a favorite, having already told the
    // user why - callers do not need to report anything.
    static bool promptAddCurrentStream();

    // Convenience for the floater: the agent parcel's trimmed music URL.
    static std::string getAgentParcelMusicURL();

    // Whether the status bar should show the "add current stream to favorites"
    // button. Owns the setting name so no Firestorm file has to know it.
    static bool showAddFavoriteStreamButton();

    // Invokes cb whenever the answer above changes, so the caller only has to
    // re-apply its layout.
    static boost::signals2::connection setAddFavoriteStreamButtonChangedCallback(
        const change_signal_t::slot_type& cb);

    // Shows/hides the status bar's add-favorite button and reclaims its width
    // when hidden, so no gap is left behind. `status_bar` is the status bar
    // panel; children are looked up by name, so this needs nothing from
    // LLStatusBar's private interface.
    //
    // `stream_toggle_visible` is passed in because the button mirrors the
    // stream toggle, which the volume-controls setting also drives.
    static void updateAddFavoriteStreamButtonLayout(LLPanel* status_bar,
                                                    bool stream_toggle_visible);

    boost::signals2::connection setFavoritesChangedCallback(const change_signal_t::slot_type& cb)
    {
        return mFavoritesChangedSignal.connect(cb);
    }

private:
    void        loadFavorites();
    void        saveFavorites() const;
    std::string getFavoritesFilename() const;
    void        onFavoritesChanged();

    void        cancelPrompt();
    static bool onPromptResponse(const LLSD& notification, const LLSD& response);

    void        startWatchdog();
    void        stopWatchdog();
    bool        onWatchdogTick();

    void        syncStatusBar(bool stream_enabled);

    // Starts a URL honoring MediaEnableFilter, exactly as the parcel code does.
    static void startStreamFiltered(const std::string& url);

    EHoldReason       mHoldReason;
    std::string       mHeldStreamURL;
    LLNotificationPtr mPrompt;

    favorites_vec_t   mFavorites;
    std::string       mFavoritesFilename;
    change_signal_t   mFavoritesChangedSignal;

    bool              mWatchdogActive;
    LLFrameTimer      mDeadStreamTimer;
};

#endif // AS_STREAMKEEPER_H
