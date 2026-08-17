# AyaneStorm Stream Keeper + Favorite Streams — Implementation Notes

**Status:** implemented, commit `cf6e70c1feda4e6aaa0c9afb606ed402fdf33b25`. This originally was a pre-build spec; it has been updated to describe what was actually built. Sections below are annotated with **Actual:** notes where the implementation diverged from the original design.

**Repo:** `e:\dev\AyaneStorm\ayanestorm-special`

---

## 0. Rules (non-negotiable)

1. **Never build.** The user compiles. Do not run cmake/msbuild/ninja.
2. **Never run git commands.**
3. **Ignore all clangd / VS Code IDE errors** — they are PCH false positives in this tree.
4. **All new files use the `as` prefix** (AyaneStorm), not `fs`.
5. **Every edit to an existing file must be wrapped in tag comments:**
   ```cpp
   // <AS:chanayane> Stream keeper
   //old line commented out with a leading //
   new line
   // </AS:chanayane>
   ```
   When only adding (not replacing), omit the commented-out part. Keep the tag text exactly `Stream keeper`.
6. **All feature logic lives in `ASStreamKeeper`.** Firestorm files get one-line hook calls only — no state, no conditionals beyond the gate call.
7. **Defaults must be vanilla.** With `ASKeepStreamAsk=0` and `ASAutoPlayDefaultFavorite=0`, behavior must be byte-identical to today.
8. Every new `.cpp` starts with `#include "llviewerprecompiledheaders.h"` as the **first** include.

---

## 1. What is being built

**Feature A — Keep previous stream.** On entering a parcel whose music URL differs from what is playing, *or which has no music URL at all*, show a dialog: keep the current stream, or take the parcel's. The current stream **keeps playing while the dialog is open** — it is only stopped/switched if the user says so. Opt-in via `ASKeepStreamAsk` (default off).

**Feature B — Favorite streams.** Per-avatar list of stream URLs. Floater to manage them, status-bar button to add the playing stream, playback by double-click or select+Play, a button to return to the parcel stream, and an optional "auto-play my default favorite instead of the parcel stream" mode.

Both need the same primitive: play a non-parcel stream and **hold** it against parcel updates.

---

## 2. Architecture — the gate

`LLViewerAudio::startInternetStreamWithAutoFade(url)` already accepts any URL. The problem is that parcel updates clobber it from ~9 sites.

**Do not** thread a source enum through `startInternetStreamWithAutoFade()`. Instead, all Firestorm hooks use one static predicate:

```cpp
// true  => parcel/media code may proceed exactly as vanilla
// false => the stream keeper is holding a stream; skip the vanilla action
static bool ASStreamKeeper::allowParcelStreamChange();
```

Implementation is one line:
```cpp
return !instanceExists() || getInstance()->mHoldReason == HOLD_NONE;
```

`instanceExists()` first means nothing is constructed when the feature is unused.

**This gate-before-act shape is also how "never interrupt while asking" works.** The prompt is raised *instead of* the stop, so the audio engine is never told to fade. There is no gap and no resume logic.

---

## 3. New file: `indra/newview/asstreamkeeper.h`

Use the standard LGPL header block from `fsfloaterprotectedfolders.h` with `@file asstreamkeeper.h`, `@brief AyaneStorm audio stream keeper and favorites`, `firstyear=2026`, and AyaneStorm attribution.

```cpp
#ifndef AS_STREAMKEEPER_H
#define AS_STREAMKEEPER_H

#include "llsingleton.h"
#include "llnotificationptr.h"
#include "llframetimer.h"
#include <boost/signals2.hpp>
#include <string>
#include <vector>

class ASFavoriteStream
{
public:
    std::string mName;
    std::string mURL;

    LLSD toLLSD() const;
    static ASFavoriteStream fromLLSD(const LLSD& data);
};

class ASStreamKeeper : public LLSingleton<ASStreamKeeper>
{
    LLSINGLETON(ASStreamKeeper);
    ~ASStreamKeeper();

public:
    enum EHoldReason
    {
        HOLD_NONE,      // not holding; vanilla parcel behavior
        HOLD_PENDING,   // prompt open; keep playing, block all parcel changes
        HOLD_KEPT,      // user answered "Keep current"
        HOLD_FAVORITE   // user is playing a favorite
    };

    typedef std::vector<ASFavoriteStream>   favorites_vec_t;
    typedef boost::signals2::signal<void()> change_signal_t;

    // ---- Hooks called from Firestorm files ----
    static bool allowParcelStreamChange();
    // parcel_url empty  => new parcel has no stream.
    // returns true      => keeper handled it; caller must not touch the stream.
    static bool handleParcelStreamChange(const std::string& parcel_url);
    static void onLoginComplete();
    static void onLogout();
    static void onTeleportStarted();
    static void onShutdown();

    // ---- Playback ----
    void playFavorite(size_t index);
    void playStreamURL(const std::string& url, EHoldReason reason);
    void playParcelStream();
    void releaseHold();

    // ---- Hold state ----
    bool               isHoldingStream() const { return mHoldReason != HOLD_NONE; }
    EHoldReason        getHoldReason() const   { return mHoldReason; }
    const std::string& getHeldStreamURL() const { return mHeldStreamURL; }

    // ---- Favorites ----
    const favorites_vec_t& getFavorites() const { return mFavorites; }
    bool addFavorite(const std::string& name, const std::string& url);
    bool editFavorite(size_t index, const std::string& name, const std::string& url);
    bool removeFavorite(size_t index);
    bool moveFavorite(size_t from, size_t to);
    bool hasFavoriteURL(const std::string& url) const;

    S32  getDefaultFavoriteIndex() const;   // -1 when unset or stale
    void setDefaultFavorite(size_t index);
    void clearDefaultFavorite();

    // Adds whatever is playing now; auto-names from stream metadata.
    bool addCurrentStreamAsFavorite();
    std::string getCurrentPlayingURL() const;

    boost::signals2::connection setFavoritesChangedCallback(const change_signal_t::slot_type& cb)
    { return mFavoritesChangedSignal.connect(cb); }

private:
    void        loadFavorites();
    void        saveFavorites() const;
    std::string getFavoritesFilename() const;
    void        onFavoritesChanged();       // saveFavorites() + fire signal

    void        cancelPrompt();
    static bool onPromptResponse(const LLSD& notification, const LLSD& response);

    void        startWatchdog();
    void        stopWatchdog();
    bool        onWatchdogTick();

    void        syncStatusBar(bool stream_enabled);
    static bool startStreamFiltered(const std::string& url); // honors MediaEnableFilter

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
```

**Actual:** `addCurrentStreamAsFavorite()` doesn't exist; replaced by static `promptAddCurrentStream()` (opens/creates the floater and calls `beginAddStream(name, url)` so the user names it before saving, rather than saving directly). Added beyond this spec: `suppressTeleportStreamFade()`, `releaseHoldIfAny()`, `removeFavoriteByURL()` (stable across list mutation, unlike index), `getAgentParcelMusicURL()`, `showAddFavoriteStreamButton()` + `setAddFavoriteStreamButtonChangedCallback()` + `updateAddFavoriteStreamButtonLayout(LLPanel*, bool)` (status-bar button visibility/layout, kept in this class per the "logic lives in AS files" rule), `getSuggestedNameForCurrentStream()` (name from URL host, not ICY metadata — see §4.8).

---

## 4. New file: `indra/newview/asstreamkeeper.cpp`

Includes needed: `llviewerprecompiledheaders.h` first, then `asstreamkeeper.h`, `llagent.h`, `llaudioengine.h`, `lldiriterator.h`, `llnotifications.h`, `llnotificationsutil.h`, `llparcel.h`, `llsdserialize.h`, `llstartup.h`, `llstatusbar.h`, `llstreamingaudio.h`, `llviewercontrol.h`, `llvieweraudio.h`, `llviewermedia.h`, `llviewerparcelmedia.h`, `llviewerparcelmgr.h`, `lldir.h`.

### 4.1 `allowParcelStreamChange()`

```cpp
// static
bool ASStreamKeeper::allowParcelStreamChange()
{
    return !instanceExists() || getInstance()->mHoldReason == HOLD_NONE;
}
```

### 4.2 `handleParcelStreamChange()` — the decision function

Order matters. Return `true` at any step that handles the change.

```cpp
// static
bool ASStreamKeeper::handleParcelStreamChange(const std::string& parcel_url)
{
    // Cheap outs first so the singleton is never built when unused.
    static LLCachedControl<bool> ask_keep(gSavedSettings, "ASKeepStreamAsk", false);
    static LLCachedControl<bool> autoplay_fav(gSavedSettings, "ASAutoPlayDefaultFavorite", false);
    if (!ask_keep && !autoplay_fav && !instanceExists())
    {
        return false;
    }

    ASStreamKeeper* self = getInstance();

    // 1. Already holding or prompting: never re-prompt, never stack.
    //    This is what lets a playing favorite continue silently across parcels.
    if (self->mHoldReason != HOLD_NONE)
    {
        return true;
    }

    // Not fully logged in: never prompt (avoids a login-time toast).
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return false;
    }

    // 2. Auto-play the default favorite instead of the parcel stream.
    if (autoplay_fav)
    {
        S32 idx = self->getDefaultFavoriteIndex();
        if (idx >= 0)
        {
            self->playFavorite((size_t)idx);
            return true;
        }
        // Stale/unset default: fall through to vanilla.
    }

    // 3. Keep-current prompt.
    if (!ask_keep || !gAudiop)
    {
        return false;
    }

    const std::string current_url = gAudiop->getInternetStreamURL();
    if (current_url.empty() || LLAudioEngine::AUDIO_PLAYING != gAudiop->isInternetStreamPlaying())
    {
        return false;   // nothing playing to keep
    }
    if (!parcel_url.empty() && parcel_url == current_url)
    {
        return false;   // same stream; nothing to ask
    }

    self->cancelPrompt();

    LLSD args;
    args["CURRENT_URL"] = current_url;
    args["PARCEL_URL"]  = parcel_url;
    LLSD payload;
    payload["current_url"] = current_url;
    payload["parcel_url"]  = parcel_url;

    // Two variants: the button labels differ and notification forms are static.
    const std::string notif = parcel_url.empty() ? "ASKeepCurrentStreamNoParcel"
                                                 : "ASKeepCurrentStream";

    self->mHoldReason    = HOLD_PENDING;   // blocks every clobber path while open
    self->mHeldStreamURL = current_url;
    self->mPrompt = LLNotificationsUtil::add(notif, args, payload,
                                             &ASStreamKeeper::onPromptResponse);
    return true;   // caller must NOT stop or switch the stream
}
```

**Critical:** use `gAudiop->getInternetStreamURL()` (what is actually playing), **not** `LLViewerAudio::getNextStreamURI()` — during a fade the latter is the pending target and gives false positives.

**Actual:** matches this shape. Order implemented: (1) `!instanceExists()` → false, (2) already holding/pending → true, (3) both settings off → false, (4) startup `< STATE_STARTED` → false, (5) `autoplay_fav` with valid default index → `playFavorite()`, return true, (6) `ask_keep` path as below.

### 4.3 `onPromptResponse()`

```cpp
// static
bool ASStreamKeeper::onPromptResponse(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    const std::string current_url = notification["payload"]["current_url"].asString();
    const std::string parcel_url  = notification["payload"]["parcel_url"].asString();

    ASStreamKeeper* self = getInstance();
    self->mPrompt = nullptr;

    if (option == 0)   // Keep current
    {
        self->mHoldReason    = HOLD_KEPT;
        self->mHeldStreamURL = current_url;
        self->syncStatusBar(true);
        self->startWatchdog();
        // Audio was never touched. Nothing else to do.
    }
    else               // Switch to parcel stream / Stop playing
    {
        self->mHoldReason = HOLD_NONE;
        self->mHeldStreamURL.clear();
        self->stopWatchdog();

        if (parcel_url.empty())
        {
            LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
            self->syncStatusBar(false);
        }
        else
        {
            startStreamFiltered(parcel_url);
            self->syncStatusBar(true);
        }
    }
    return false;
}
```

**Actual:** matches, with `syncStatusBar(true)` called *before* `startStreamFiltered(parcel_url)` on the switch branch — the toggle shows "playing" optimistically even though the filter may itself prompt/deny; safe because `LLStatusBar::refresh()` re-derives the real state from `gAudiop` every frame.

### 4.4 Playback helpers

```cpp
// static — honors MediaEnableFilter exactly as vanilla parcel code does.
bool ASStreamKeeper::startStreamFiltered(const std::string& url)
{
    if (gSavedSettings.getBOOL("MediaEnableFilter"))
    {
        LLViewerParcelMedia::getInstance()->filterAudioUrl(url);
    }
    else
    {
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);
    }
    return true;
}
```

**Actual:** before calling `filterAudioUrl(url)`, clears `LLViewerParcelMedia::mCurrentMusic` and `mAudioLastURL`. Both are fast-path caches that skip the prompt/filter entirely when they match `url`; they only get refreshed by the vanilla parcel-media flow, which the keep-prompt path bypasses — so without the clear, switching to a previously-filtered URL would auto-play with no filter prompt.

```cpp
void ASStreamKeeper::playStreamURL(const std::string& url, EHoldReason reason)
{
    if (url.empty()) return;

    cancelPrompt();
    mHoldReason    = reason;
    mHeldStreamURL = url;

    // Favorites are user-chosen and therefore trusted: bypass the media filter,
    // which exists to vet parcel-supplied URLs. Matches /music <url> behavior.
    LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);

    syncStatusBar(true);
    startWatchdog();
}

void ASStreamKeeper::playFavorite(size_t index)
{
    if (index >= mFavorites.size()) return;
    playStreamURL(mFavorites[index].mURL, HOLD_FAVORITE);
}

void ASStreamKeeper::playParcelStream()
{
    cancelPrompt();
    mHoldReason = HOLD_NONE;        // release BEFORE playing so parcel logic resumes
    mHeldStreamURL.clear();
    stopWatchdog();

    std::string url;
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (parcel)
    {
        url = parcel->getMusicURL();
        LLStringUtil::trim(url);
    }

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
}

void ASStreamKeeper::releaseHold()
{
    cancelPrompt();
    mHoldReason = HOLD_NONE;
    mHeldStreamURL.clear();
    stopWatchdog();
}
```

Note `getAgentParcel()` is null-checked. Do **not** call `LLViewerMedia::getParcelAudioURL()` — it dereferences `getAgentParcel()` without a null check ([llviewermedia.cpp:1597](../indra/newview/llviewermedia.cpp#L1597)).

### 4.5 `syncStatusBar()`

`LLStatusBar::mAudioStreamEnabled` is read at `llviewerparcelmgr.cpp:2148` as the "user wants streams" signal. Any path that starts audio outside the status bar must set it, or the next parcel change stops the stream.

```cpp
void ASStreamKeeper::syncStatusBar(bool stream_enabled)
{
    if (gStatusBar)
    {
        gStatusBar->setAudioStreamEnabled(stream_enabled);
    }
}
```

### 4.6 Watchdog

There is no reliable synchronous failure callback for stream URLs. Without this, a dead held stream silently blocks all parcel music forever.

`startWatchdog()` sets `mWatchdogActive = true`, resets `mDeadStreamTimer`, and registers `doOnIdleRepeating(boost::bind(&ASStreamKeeper::onWatchdogTick, this))`. `onWatchdogTick()` returns `true` (deregister) when `!mWatchdogActive`. While active: if `gAudiop && AUDIO_STOPPED == gAudiop->isInternetStreamPlaying()`, and `mDeadStreamTimer` has run past ~10s, call `releaseHold()`, `LL_INFOS` it, and return `true`. Reset the timer whenever the stream is not stopped.

### 4.7 Lifecycle statics

```cpp
// static
void ASStreamKeeper::onLoginComplete()  { getInstance()->loadFavorites(); }

// static
void ASStreamKeeper::onLogout()
{
    if (!instanceExists()) return;
    ASStreamKeeper* self = getInstance();
    self->releaseHold();
    self->mFavorites.clear();
    self->mFavoritesFilename.clear();   // MUST clear: prevents account A's list
                                        // overwriting account B's file on relog
    self->mFavoritesChangedSignal();
}

// static
void ASStreamKeeper::onTeleportStarted()
{
    if (instanceExists()) getInstance()->cancelPrompt();
}

// static
void ASStreamKeeper::onShutdown()
{
    if (instanceExists()) getInstance()->saveFavorites();
}
```

`cancelPrompt()` must mirror `LLViewerParcelAskPlay::cancelNotification()` ([llviewerparcelaskplay.cpp:108-121](../indra/newview/llviewerparcelaskplay.cpp#L108)): if `mPrompt` is set and not yet responded to, call `mPrompt->setIgnored(false)` then `LLNotifications::getInstance()->cancel(mPrompt)`, then null it. **Also reset `mHoldReason` to `HOLD_NONE` if it was `HOLD_PENDING`** — a cancelled prompt must not leave a permanent hold.

**Actual:** `onTeleportStarted()` also calls `suppressTeleportStreamFade()` from the llvieweraudio.cpp hooks (see §5.2) — a teleport starts before the destination parcel is known, so no prompt can exist yet to gate on; the fade is suppressed pre-emptively whenever a prompt is *possible* on arrival, not just when one is open.

### 4.8 Favorites persistence

**Filename — two-step probe is mandatory.** Before login `LL_PATH_PER_SL_ACCOUNT` resolves to empty, and `getExpandedFilename` would return a bare filename that lands in the CWD.

```cpp
std::string ASStreamKeeper::getFavoritesFilename() const
{
    std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "");
    if (!path.empty())
    {
        path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "as_favorite_streams.xml");
    }
    return path;
}
```

`loadFavorites()`: resolve and store `mFavoritesFilename`; return if empty; return if `!gDirUtilp->fileExists(...)`; `llifstream` + `if (LLSDSerialize::fromXML(data, stream) >= 1 && data.isArray())`; push each `fromLLSD()` whose URL is non-empty; fire the signal.

`saveFavorites()`: return if `mFavoritesFilename.empty()`; build an `LLSD` array of `toLLSD()`; `llofstream` + `LLSDSerialize::toPrettyXML`.

The `>= 1` return check is the established local idiom — see [fsassetblacklist.cpp:303](../indra/newview/fsassetblacklist.cpp#L303).

File format:
```xml
<llsd><array>
  <map><key>name</key><string>Radio Paradise</string>
       <key>url</key><string>http://stream.example/aac</string></map>
</array></llsd>
```

**Every mutator** (`addFavorite`, `editFavorite`, `removeFavorite`, `moveFavorite`, `setDefaultFavorite`, `clearDefaultFavorite`) ends by calling `onFavoritesChanged()` = `saveFavorites(); mFavoritesChangedSignal();`. `addFavorite` rejects empty URLs and duplicates (`hasFavoriteURL`).

**Default favorite is stored by URL, not index** — indices shift on reorder/remove:
```cpp
S32 ASStreamKeeper::getDefaultFavoriteIndex() const
{
    const std::string url = gSavedPerAccountSettings.getString("ASDefaultFavoriteStreamURL");
    if (url.empty()) return -1;
    for (size_t i = 0; i < mFavorites.size(); ++i)
        if (mFavorites[i].mURL == url) return (S32)i;
    return -1;   // stale (favorite deleted) -> graceful fallback to vanilla
}
```

**Actual:** `addCurrentStreamAsFavorite()` doesn't exist. `getSuggestedNameForCurrentStream()` derives the name from the URL host only (scheme-strip + first `/`) — deliberately **not** `getStreamingAudioImpl()->getCurrentMetadata()`/ICY track metadata, which names the song playing right now, not the station, and would bake a stale track name into the entry. `promptAddCurrentStream()` (static): if `getCurrentPlayingURL()` is empty or already a favorite, shows the `ASFavoriteStreamNotAdded` notification and returns false; otherwise opens/creates the `as_favoritestreams` floater and calls `beginAddStream(name, url)` on it so the user can rename before saving — it does not call `addFavorite()` directly.

---

## 5. Firestorm file edits

### 5.1 `indra/newview/llviewerparcelmgr.cpp`

Add `#include "asstreamkeeper.h"` in a tagged block with the other includes.

**Edit 1 — the parcel-music block (currently lines 2034-2074).** This is the most important edit. Three separate branches stop the stream (valid-URL → `optionallyStartMusic`, invalid-URL at :2064, empty-URL at :2071); one gate covers all three.

The URL trim must be hoisted above the gate so the keeper receives it. It is a pure string operation, safe to move.

Current structure:
```cpp
if (parcel)
{
    // Only update stream if parcel changed (recreated) or music is playing (enabled)
    static LLCachedControl<bool> already_playing(gSavedSettings, "MediaTentativeAutoPlay", true);
    if (!agent_parcel_update || already_playing)
    {
        LLViewerParcelAskPlay::getInstance()->cancelNotification();
        std::string music_url_raw = parcel->getMusicURL();
        std::string music_url = music_url_raw;
        LLStringUtil::trim(music_url);
        ...three branches...
    }
}
```

Rewrite as:
```cpp
if (parcel)
{
    // Only update stream if parcel changed (recreated) or music is playing (enabled)
    static LLCachedControl<bool> already_playing(gSavedSettings, "MediaTentativeAutoPlay", true);
    if (!agent_parcel_update || already_playing)
    {
// <AS:chanayane> Stream keeper
        // Compute the effective URL up front so the stream keeper can decide.
        // Empty here means "this parcel has no usable stream".
        std::string as_effective_url = parcel->getMusicURL();
        LLStringUtil::trim(as_effective_url);
        if (as_effective_url.size() <= 12
            || (as_effective_url.substr(0, 7) != "http://"
                && as_effective_url.substr(0, 8) != "https://"))
        {
            as_effective_url.clear();
        }
        if (!ASStreamKeeper::handleParcelStreamChange(as_effective_url))
        {
// </AS:chanayane>
        LLViewerParcelAskPlay::getInstance()->cancelNotification();
        std::string music_url_raw = parcel->getMusicURL();

        // Trim off whitespace from front and back
        std::string music_url = music_url_raw;
        LLStringUtil::trim(music_url);

        ... existing three branches, completely unchanged ...
// <AS:chanayane> Stream keeper
        }
// </AS:chanayane>
    }
}
```

The existing body is left byte-identical inside the new `if`. The duplicated trim is intentional and harmless.

**Edit 2 — public land branch (currently 2075-2080).**
```cpp
else
{
// <AS:chanayane> Stream keeper
    if (!ASStreamKeeper::handleParcelStreamChange(LLStringUtil::null))
    {
// </AS:chanayane>
    // Public land has no music
    LLViewerParcelAskPlay::getInstance()->cancelNotification();
    LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
// <AS:chanayane> Stream keeper
    }
// </AS:chanayane>
}
```

**Edit 3 — `optionallyStartMusic()`, immediately before the `MediaEnableFilter` check at line 2154.** Second line of defence for the autoplay path; unreachable when Edit 1 already gated, so it cannot double-prompt.
```cpp
if (gStatusBar->getAudioStreamEnabled() ||
   (gSavedSettings.getBOOL("FSParcelMusicAutoPlay")
            && tentative_autoplay))
{
// <AS:chanayane> Stream keeper
    if (ASStreamKeeper::handleParcelStreamChange(music_url))
    {
        return;
    }
// </AS:chanayane>
    if (gSavedSettings.getBOOL("MediaEnableFilter"))
    ...
```

Do **not** touch or revive the commented-out LL ask-mode block at lines 2114-2141.

### 5.2 `indra/newview/llvieweraudio.cpp`

Add a tagged `#include "asstreamkeeper.h"`.

**`onTeleportStarted()` (line 316)** — must gate here, not only via §2, because this function calls `setNextStreamURI(null)` and `setForcedTeleportFade(true)` directly. Insert as the first statements of the function body:
```cpp
// <AS:chanayane> Stream keeper
    ASStreamKeeper::onTeleportStarted();   // dismiss any open prompt
    if (!ASStreamKeeper::allowParcelStreamChange())
    {
        return;   // keep the held stream alive across the teleport
    }
// </AS:chanayane>
```

**`onTeleportFailed()` (line 351)** — change
`if (gAudiop && mWasPlaying)` → tagged replacement adding `&& ASStreamKeeper::allowParcelStreamChange()`. Leave `mWasPlaying = false;` outside the guard.

**`onTeleportFinished()` (line 369)** — same, on `if (gAudiop && local && mWasPlaying)`.

**Actual:** all three conditions also add `&& !ASStreamKeeper::suppressTeleportStreamFade()`. Reason: `onTeleportStarted()` fires before the destination parcel is known, so `mHoldReason` is still `HOLD_NONE` and `allowParcelStreamChange()` alone would let the fade run, killing the stream the keep-prompt is about to offer to keep. `suppressTeleportStreamFade()` returns true pre-emptively whenever a prompt is *possible* on arrival (holding, or `ASKeepStreamAsk` on + something actually `AUDIO_PLAYING` + logged in), not just when one is open.

### 5.3 `indra/newview/llviewermedia.cpp`

Add a tagged `#include "asstreamkeeper.h"`.

**Lines 857 and 866** — the `restore_parcel_audio` proximity path stops then restores parcel audio. Add `&& ASStreamKeeper::allowParcelStreamChange()` to both conditions (tagged).

**Line ~1059, `setAllMediaEnabled(true)` branch** — add the same clause to the condition (the bare `gAudiop->pauseInternetStream(false)` inside bypasses `LLViewerAudio`, so gating the condition is the only fix).

**The disable branch (~line 1079)** — "stop all media" is a deliberate user act, so release first:
```cpp
// <AS:chanayane> Stream keeper
    ASStreamKeeper::getInstance()->releaseHold();
// </AS:chanayane>
    LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
```

**Actual:** matches this shape (uses `releaseHoldIfAny()`, the null-safe static, rather than `getInstance()->releaseHold()`).

### 5.4 `indra/newview/llstatusbar.h`

Add a tagged setter next to `getAudioStreamEnabled()` (line 147):
```cpp
// <AS:chanayane> Stream keeper
    void setAudioStreamEnabled(bool enabled) { mAudioStreamEnabled = enabled; }
// </AS:chanayane>
```
Also declare `LLButton* mAddFavoriteStreamBtn;` (tagged) near the other button members, and a `static void onClickAddFavoriteStream(void* data);`.

**Actual:** the setter matches. **No `mAddFavoriteStreamBtn` member exists** — the button is looked up ad hoc by name (`findChild<LLButton>("as_add_favorite_stream_btn")`) inside `ASStreamKeeper::updateAddFavoriteStreamButtonLayout()`, keeping that logic in the AS class rather than LLStatusBar. Also added: private `void updateAddFavoriteStreamButtonVisibility()`, a thin one-line wrapper that forwards to `ASStreamKeeper::updateAddFavoriteStreamButtonLayout(this, ...)`.

### 5.5 `indra/newview/llstatusbar.cpp`

Add a tagged `#include "asstreamkeeper.h"`.

**`refresh()` lines 786-789 — required fix.** Currently:
```cpp
button_enabled = (audio_streaming_music && media_inst->hasParcelAudio());
mStreamToggle->setEnabled(button_enabled);
mStreamToggle->setValue(!media_inst->isParcelAudioPlaying());
```
Both helpers derive from the **parcel** URL, so a held stream on a parcel with no music URL greys the button out and shows "stopped" while audio plays. Since "keep a stream on a parcel with none" is an explicitly supported state, this must be fixed:
```cpp
// <AS:chanayane> Stream keeper
//  button_enabled = (audio_streaming_music && media_inst->hasParcelAudio());
//  mStreamToggle->setEnabled(button_enabled);
//  mStreamToggle->setValue(!media_inst->isParcelAudioPlaying());
    bool as_holding = ASStreamKeeper::instanceExists()
                      && ASStreamKeeper::getInstance()->isHoldingStream();
    button_enabled = (audio_streaming_music && (media_inst->hasParcelAudio() || as_holding));
    mStreamToggle->setEnabled(button_enabled);
    bool as_playing = media_inst->isParcelAudioPlaying()
                      || (as_holding && gAudiop
                          && LLAudioEngine::AUDIO_PLAYING == gAudiop->isInternetStreamPlaying());
    mStreamToggle->setValue(!as_playing);
// </AS:chanayane>
```

**`toggleStream()` line 1188.** Two tagged edits:
- Enable branch: when holding, use `ASStreamKeeper::getInstance()->getHeldStreamURL()` instead of `LLViewerMedia::getInstance()->getParcelAudioURL()`, and skip the `MediaEnableFilter` path for that URL.
- Stop branch (line 1216): call `ASStreamKeeper::getInstance()->releaseHold();` **before** `stopInternetStreamWithAutoFade()`. Stop is unambiguous intent for silence and must not leave a zombie hold.

**New button.** Wire `mAddFavoriteStreamBtn` in `postBuild()` beside `mStreamToggle`, with handler:
```cpp
// static
void LLStatusBar::onClickAddFavoriteStream(void* data)
{
    ASStreamKeeper::getInstance()->addCurrentStreamAsFavorite();
}
```
Set its visibility from `ASShowAddFavoriteStreamButton`, and include it wherever `mStreamToggle` is hidden for mouselook.

**Actual, all of §5.5:**
- `toggleStream()` matches the described shape (uses `releaseHoldIfAny()`).
- `onClickAddFavoriteStream(void*)` is a one-liner forwarding to the static `ASStreamKeeper::promptAddCurrentStream()`, not `addCurrentStreamAsFavorite()`.
- `refresh()`: same OR-in-holding-state fix as spec, implemented via `ASStreamKeeper::isHoldingStream()`.
- Constructor connects `ASStreamKeeper::setAddFavoriteStreamButtonChangedCallback(boost::bind(&LLStatusBar::updateAddFavoriteStreamButtonVisibility, this))` so the button's setting can change layout without a poll.
- `postBuild()`: wires the button's click callback, then calls `updateAddFavoriteStreamButtonVisibility()` once explicitly — needed because that call also happens inside the `FSEnableVolumeControls` block, which only runs when volume controls are hidden.
- `updateVolumeControlsVisibility()` and the mouselook-hide path both also call `updateAddFavoriteStreamButtonVisibility()`, so the button's width is always correctly reclaimed/restored alongside the other icons.
- `updateAddFavoriteStreamButtonLayout()` (in asstreamkeeper.cpp) finds the button by name, sets `visible = stream_toggle_visible && showAddFavoriteStreamButton()`, and reclaims width by shifting **`stream_toggle_btn`** — the sibling to the button's *left* in `panel_status_bar.xml` — right when hidden, left when shown, by `button_width + 2`px. (`media_toggle_btn`, `volume_btn`, `FPSText` sit to the button's right and stay fixed, since `time_and_media_bg`'s right-edge anchor is effectively pinned by them.) A `static S32 applied_offset` prevents double-shifting on repeated calls.

### 5.6 Registration and lifecycle

**`llviewerfloaterreg.cpp`** — tagged `#include "asfloaterfavoritestreams.h"` near line 215; tagged registration near line 655:
```cpp
LLFloaterReg::add("as_favoritestreams", "floater_as_favoritestreams.xml",
    (LLFloaterBuildFunc)&LLFloaterReg::build<ASFloaterFavoriteStreams>);
```

**`llstartup.cpp`** — tagged call right after the `LLViewerParcelAskPlay::loadSettings()` block at lines 2328-2331 (this is inside `STATE_STARTED`, post-login, which is required):
```cpp
// <AS:chanayane> Stream keeper
        ASStreamKeeper::onLoginComplete();
// </AS:chanayane>
```

**`llappviewer.cpp`** — tagged call right after the `LLViewerParcelAskPlay` block at lines 2348-2351 (inside the `else` that already guarantees a valid per-account filename):
```cpp
// <AS:chanayane> Stream keeper
        ASStreamKeeper::onShutdown();
// </AS:chanayane>
```
Also call `ASStreamKeeper::onLogout()` on the disconnect/cleanup path (search for where other per-account singletons are reset on logout).

**`CMakeLists.txt`** — add alphabetically, tagged:
- `viewer_SOURCE_FILES` (~line 142): `asfloaterfavoritestreams.cpp`, `asstreamkeeper.cpp`
- `viewer_HEADER_FILES` (~line 1005): `asfloaterfavoritestreams.h`, `asstreamkeeper.h`

XUI files need **no** CMake entry.

---

## 6. Notifications

Add both to `indra/newview/skins/default/xui/en/notifications.xml` after the `ParcelPlayingMedia` block (ends ~line 7020), wrapped in tag comments. Two notifications because form button labels are static.

```xml
<!-- <AS:chanayane> Stream keeper -->
  <notification
   icon="notify.tga"
   name="ASKeepCurrentStream"
   label="Keep Current Audio Stream"
   persist="false"
   type="notify">
This parcel plays a different audio stream. Would you like to keep listening to the current stream?

Current: [CURRENT_URL]
Parcel: [PARCEL_URL]
    <tag>confirm</tag>
    <form name="form">
      <button default="true" index="0" name="Keep" text="Keep current"/>
      <button index="1" name="Switch" text="Switch to parcel stream"/>
    </form>
  </notification>

  <notification
   icon="notify.tga"
   name="ASKeepCurrentStreamNoParcel"
   label="Keep Current Audio Stream"
   persist="false"
   type="notify">
This parcel has no audio stream. Would you like to keep listening to the current stream?

Current: [CURRENT_URL]
    <tag>confirm</tag>
    <form name="form">
      <button default="true" index="0" name="Keep" text="Keep current"/>
      <button index="1" name="Stop" text="Stop playing"/>
    </form>
  </notification>
<!-- </AS:chanayane> -->
```

`default="true"` on Keep means ESC/dismiss preserves what is playing. No `<ignore>` checkbox — `ASKeepStreamAsk` is the global opt-out.

**Actual:** both notifications ended up `type="alertmodal"` with `usetemplate name="okcancelbuttons"` (`yestext="Keep current"`, `notext="Switch to parcel stream"` / `"Stop playing"`), not `type="notify"` — a toast doesn't size to content, and with unwrapped long URLs in the body it rendered with buttons drawn over the text. No `default="true"`/`<ignore>` on these two either way. Three more notifications exist beyond this spec: `ASFavoriteStreamNotAdded` (okbutton, fired by `promptAddCurrentStream()` when nothing is playing or it's already a favorite), `ASConfirmRemoveFavoriteStream` (okcancelignore, keyed by URL not index so it can't go stale), and `ASEnableAutoPlayDefaultFavorite` (okcancelbuttons, offered when setting a default while the autoplay setting is off).

---

## 7. Settings

**`indra/newview/app_settings/settings.xml`** — three Boolean entries, tagged, matching the shape of neighbouring entries (`Comment`, `Persist`=1, `Type`=Boolean, `Value`):

| Key | Value | Comment |
|---|---|---|
| `ASKeepStreamAsk` | **0** | Ask whether to keep the current audio stream when entering a parcel with a different stream or no stream |
| `ASAutoPlayDefaultFavorite` | **0** | Play the default favorite stream instead of the parcel stream |
| `ASShowAddFavoriteStreamButton` | 1 | Show the 'add stream to favorites' button in the status bar |

**`indra/newview/app_settings/settings_per_account.xml`** — one String entry, tagged:

| Key | Value | Comment |
|---|---|---|
| `ASDefaultFavoriteStreamURL` | `""` | URL of the favorite stream to auto-play (stored by URL so reordering is safe) |

**`indra/newview/skins/default/xui/en/panel_preferences_ayanestorm.xml`** — add the three visible checkboxes here, **not** in the sound panel. This panel has **no registered C++ class** (`panel_preference_ayanestorm` has no `LLRegisterPanelClass` match), so it uses generic `LLPanelPreference` and plain `control_name` widgets work with **zero C++**. Follow the existing `<check_box control_name="ASEnableUnencryptedCache" ...>` at line 14 as the template, but **without** a `commit_callback`. Place them in an "Audio streams" group below the existing cache controls, using `top_pad` to flow after the last widget.

`ASDefaultFavoriteStreamURL` is **not** exposed here — it is set from the floater.

---

## 8. Floater

### 8.1 `indra/newview/asfloaterfavoritestreams.h`

Model exactly on `fsfloaterprotectedfolders.h`. Pure view — no feature state.

```cpp
class ASFloaterFavoriteStreams : public LLFloater
{
public:
    ASFloaterFavoriteStreams(const LLSD& key);
    virtual ~ASFloaterFavoriteStreams();

    bool postBuild() override;
    void onOpen(const LLSD& info) override;
    void draw() override;

private:
    void updateList();
    void onSelectionChanged();
    void onDoubleClick();
    void onPlay();
    void onPlayParcel();
    void onAddCurrent();
    void onEdit();
    void onRemove();
    void onSetDefault();
    void onClearDefault();
    void onSaveEdit();
    void onCancelEdit();
    void showEditPanel(bool show, S32 edit_index);

    boost::signals2::connection mFavoritesChangedConnection;

    LLScrollListCtrl* mStreamList;
    LLButton* mPlayBtn; /* + mPlayParcelBtn, mAddBtn, mEditBtn, mRemoveBtn,
                           mSetDefaultBtn, mClearDefaultBtn, mSaveBtn, mCancelBtn */
    LLLineEditor* mNameEditor;
    LLLineEditor* mUrlEditor;
    LLPanel* mEditPanel;
    S32 mEditIndex;   // -1 == adding
    bool mInitialized;
};
```

**Actual:** matches, plus `mListPanel` and `mButtonsPanel` members (the edit panel and the buttons panel are mutually exclusive; the list panel's bottom edge is resized to clear whichever is visible) and `mAutoPlayChangedConnection` (rebuilds the list when `ASAutoPlayDefaultFavorite` is toggled elsewhere, e.g. Preferences). Also overrides `reshape()` to re-apply the list-bounds fixup after `LLFloater::reshape()`, since XUI's own follows-rules would otherwise restore the list's original bottom edge on resize.

### 8.2 `indra/newview/asfloaterfavoritestreams.cpp`

- `postBuild()`: `getChild<>` all widgets; `mStreamList->setDoubleClickCallback(...)`; `setCommitCallback` for selection change; wire every button; hide the edit panel.
- `onOpen()`: connect `ASStreamKeeper::setFavoritesChangedCallback(boost::bind(&ASFloaterFavoriteStreams::updateList, this))` once, guarded by `mInitialized`; call `updateList()`.
- Destructor: **disconnect `mFavoritesChangedConnection`**.
- `updateList()`: remember `mStreamList->getSelectedValue()`; `clearRows()`; wrap insertion with `setNeedsSort(false)` … `setNeedsSort(needs_sort); updateSort();`; add one row per favorite with `row["value"] = (S32)index` and two columns `name` / `url`; mark the default favorite (prefix the name with `"* "`); restore the selection.
- `draw()`: enable/disable Play, Edit, Remove, Set default from `mStreamList->getNumSelected()`; enable Play parcel only when the agent parcel has a non-empty music URL; enable Clear default only when one is set.
- `onPlay()` / `onDoubleClick()` → `ASStreamKeeper::getInstance()->playFavorite(index)`.
- `onPlayParcel()` → `ASStreamKeeper::getInstance()->playParcelStream()`.
- `onAddCurrent()` → `addCurrentStreamAsFavorite()`; if it returns false, show a message that nothing is playing.
- `onEdit()` → `showEditPanel(true, index)` prefilled; `onSaveEdit()` → `addFavorite` when `mEditIndex < 0`, else `editFavorite`.
- No per-row play buttons — `LLScrollListCtrl` has no clean support; double-click plus the Play button cover it.

**Actual:** `addCurrentStreamAsFavorite()` doesn't exist. `onAddCurrent()` does the empty/`hasFavoriteURL` check inline (duplicating `ASStreamKeeper::promptAddCurrentStream()`'s logic rather than calling it, since that static also does an `LLFloaterReg::showInstance` this floater is already open as) and calls `beginAddStream(name, url)` to prefill the edit panel. Row label uses `[NAME] (default)` / `[NAME] (default, off)` floater strings with bold styling, not a `"* "` prefix. Setting a default while `ASAutoPlayDefaultFavorite` is off raises `ASEnableAutoPlayDefaultFavorite` to offer turning the setting on — not in the original spec. `showEditPanel()`/`updateListBounds()` toggle `mButtonsPanel`/`mEditPanel` mutual visibility and resize `mListPanel` to fit.

### 8.3 `indra/newview/skins/default/xui/en/floater_as_favoritestreams.xml`

Root `<floater name="as_favoritestreams" title="Favorite Streams" single_instance="true" reuse_instance="true" save_rect="true" save_visibility="true" positioning="cascading" can_resize="true" ...>`.

Contains: `<scroll_list name="stream_list" multi_select="false" draw_heading="true" draw_stripes="true">` with columns `name` (dynamic_width) and `url`; a button row (`play`, `play_parcel`, `add_current`, `edit`, `remove`, `set_default`, `clear_default`); and a `<panel name="edit_panel" visible="false">` holding `name_editor` / `url_editor` line editors plus `save` / `cancel`.

Floater-local strings go inline as `<floater.string name="...">`, not in `strings.xml`.

**Actual:** scroll list has only a `name` column (URL shown as tool_tip, not a second column). Button row is Play, Play parcel stream, Set as default, Clear default, Add playing, Add..., Edit, Remove — "Add playing" (current stream) and "Add..." (blank form) are separate buttons rather than one `add_current`.

### 8.4 Menu entry

`indra/newview/skins/default/xui/en/menu_viewer.xml`, tagged, using the generic handlers — **no C++ needed**:
```xml
<!-- <AS:chanayane> Stream keeper -->
<menu_item_check label="Favorite Streams" name="Favorite Streams">
    <menu_item_check.on_check function="Floater.Visible" parameter="as_favoritestreams" />
    <menu_item_check.on_click  function="Floater.Toggle"  parameter="as_favoritestreams" />
</menu_item_check>
<!-- </AS:chanayane> -->
```

### 8.5 Status bar XML

`indra/newview/skins/default/xui/en/panel_status_bar.xml` — add the add-favorite button next to `stream_toggle`, tagged, copying that button's image idiom (`image_unselected="Toolbar_Middle_Off"` etc.).

---

## 9. Implementation order

1. `asstreamkeeper.h` / `.cpp` (§3, §4) — everything else depends on it.
2. Settings (§7) — needed before anything reads them.
3. Notifications (§6).
4. Firestorm hooks (§5.1-5.3) — **test the regression case here before continuing.**
5. Status bar (§5.4, §5.5, §8.5).
6. Floater (§8) + registration (§5.6).
7. CMakeLists + menu.

---

## 10. Verification (manual, in viewer — the user builds and runs)

**Regression first, with defaults (`ASKeepStreamAsk=0`, `ASAutoPlayDefaultFavorite=0`).** Walk between parcels with music, without music, and onto public land; teleport; toggle the status-bar stream button; toggle "stop all media"; test `MediaEnableFilter` on and off. Must be indistinguishable from a build without the feature. **This is the most important test.**

**No-interruption.** Enable `ASKeepStreamAsk`. With music playing, walk to a parcel with a different stream → dialog appears and **the stream keeps playing with no gap or fade**. Leave the dialog up 30s to confirm. **Keep** → still uninterrupted. Repeat and answer **Switch** → only then does the parcel stream take over.

**Parcel with no stream.** With music playing, walk onto a parcel with no music URL, and separately onto public land → "This parcel has no audio stream" prompt, audio continues. **Keep** → audio continues; status-bar toggle stays enabled and shows "playing". **Stop playing** → audio stops. Also test a parcel whose music URL is not `http://`/`https://`.

**Teleport with dialog open.** Trigger the prompt, then teleport before answering → stream is not faded out mid-dialog, prompt dismisses cleanly.

**Favorites.** Open the floater from the menu. With a stream playing, click the status-bar add button → entry appears with a sensible auto-filled name; an open floater updates live. Edit, remove, reorder → `as_favorite_streams.xml` in the per-account dir reflects each change immediately. Play by **double-click** and by **select + Play**. Relog → list persists. Log into a **second account** → separate, empty list; edit it and confirm account one's file is untouched.

**Favorite continuity.** With `ASKeepStreamAsk` on, play a favorite, then cross several parcels (with music, without music) and teleport → plays continuously, **no prompt ever appears**.

**Return to parcel.** While a favorite plays, click `Play parcel stream` → parcel audio starts, hold releases, next parcel switches normally.

**Default favorite.** Set a default, enable `ASAutoPlayDefaultFavorite`, teleport to a parcel with music → the favorite plays instead, no prompt. Delete the default favorite, teleport again → falls back to parcel music, no error.

**Preferences.** All three checkboxes appear in the **AyaneStorm** tab and take effect without restart.

**Extraction.** Reverting every `// <AS:chanayane> Stream keeper` block and deleting the five `as*` files must return the tree to vanilla with no orphaned references.

---

## 11. Pitfalls (each one has already bitten this design)

- **`llstatusbar.cpp:786-789` is load-bearing.** Skip it and both features look broken on music-less parcels — the most likely bug report.
- **Three stop-branches, not one.** `llviewerparcelmgr.cpp` stops the stream at :2064 (invalid URL), :2071 (empty URL) and :2079 (public land). Only the valid-URL path reaches `optionallyStartMusic()`. Gating only that function silently misses the no-stream cases.
- **`llviewermedia.cpp:857/866`** is a clobber site invisible from the parcel manager. Missing it causes intermittent, hard-to-reproduce hold failures.
- **`HOLD_PENDING` is required.** A parcel change often accompanies a teleport; without it, `LLViewerAudio::onTeleportStarted()` fades the stream out from under an open dialog.
- **`getNextStreamURI()` is not what's playing.** During a fade it is the pending target. Use `gAudiop->getInternetStreamURL()`.
- **`LL_PATH_PER_SL_ACCOUNT` is empty before login.** Without the two-step probe, the favorites file lands in the CWD.
- **`onLogout()` must clear `mFavoritesFilename`.** Otherwise account A's list overwrites account B's file on the first edit after a relog.
- **`LLViewerMedia::getParcelAudioURL()` has no null check** on `getAgentParcel()` ([llviewermedia.cpp:1597](../indra/newview/llviewermedia.cpp#L1597)). Use `getAgentParcel()` directly with a null check.
- **`cancelPrompt()` must clear `HOLD_PENDING`**, or a dismissed dialog leaves a permanent hold.
- **Do not touch `LLViewerParcelMedia::filterAudioUrl()`'s signature.** Favorites bypass the filter by design; the prompt is raised before the filter is reached.
- **A teleport starts before the destination parcel is known.** `onTeleportStarted()` can't gate on `HOLD_PENDING` alone since no prompt can exist yet — `suppressTeleportStreamFade()` was added to hold the fade pre-emptively whenever a prompt is *possible* on arrival.
- **`filterAudioUrl()`'s fast-path caches (`mCurrentMusic`, `mAudioLastURL`) go stale under the keep-prompt path.** They're normally refreshed by the vanilla parcel-media flow, which the prompt bypasses; without clearing them in `startStreamFiltered()`, answering "Switch" to a previously-seen URL replays it with no filter prompt.
- **Status-bar layout math must not touch the panel's own right-edge anchors.** An early version shifted `media_toggle_btn`/`volume_btn`/`FPSText` (which anchor `time_and_media_bg` to the window edge) and in the wrong direction, clipping the bar. The fix shifts only `stream_toggle_btn` — the button's left-hand sibling — and in the direction that keeps the right-side widgets fixed.
- **Notification `type="notify"` doesn't size to content.** Long unwrapped URLs in the body overlapped the buttons; switched both keep-stream prompts to `type="alertmodal"` + `okcancelbuttons`.
