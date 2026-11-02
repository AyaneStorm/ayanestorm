# AyaneStorm UI Customization Options (Login Screen, Loading Screen, Branding)

Research notes on what can be branded or customized in the viewer front-end: the login screen, the loading/progress screen, skins and general branding. Options are ordered from cheapest and most merge-safe to most invasive.

## Login Screen

### Web login page (splash screen)

- The center of the login screen is a CEF web browser (`LLMediaCtrl` named `login_html`).
- The default URL is `DEFAULT_LOGIN_PAGE` in `indra/newview/fsgridhandler.cpp` (line ~154). It is already set to `https://storm.ayane.li/`; the original Firestorm URL `https://phoenixviewer.com/app/loginV3/` is commented out.
- `LLGridManager::getLoginPage()` (`fsgridhandler.cpp`, line ~1164) returns the `LoginPage` setting first if it is not empty. This allows testing a local or staging page without rebuilding.
- Changing the page content requires only server-side work. No viewer rebuild.

### Query parameters sent to the login page

Built in `FSPanelLogin::loadLoginPage()` (`indra/newview/fspanellogin.cpp`, lines ~877-963). Besides grid, version, channel, language and `login_content_version` (setting `LoginContentVersion`), the viewer passes the splash screen options:

| Setting | Query parameter |
|---|---|
| `FSSplashScreenHideTopBar` | `hidetopbar` |
| `FSSplashScreenHideBlogs` | `hideblogs` |
| `FSSplashScreenHideDestinations` | `hidedestinations` |
| `FSSplashScreenUseGrayMode` | `usegraymode` |
| `FSSplashScreenUseHighContrast` | `usehighcontrast` |
| `FSSplashScreenUseAllCaps` | `useallcaps` |
| `FSSplashScreenUseLargerFonts` | `uselargerfonts` |
| `FSSplashScreenNoTransparency` | `notransparency` |

- The page at `storm.ayane.li` can read these and adapt its layout.
- Users toggle them in `floater_fs_splash_screen_settings.xml`. These options can be renamed or extended for the AyaneStorm page. New settings must use the `AS` prefix.

### Login panel layout (XUI)

The fields, buttons and layout around the web page are XUI only, with no C++ involved:

- `skins/default/xui/en/panel_fs_login.xml`: classic login panel
- `skins/default/xui/en/panel_fs_nui_login.xml`: newer login panel
- `skins/default/xui/en/menu_login.xml`: login menu bar
- `panel_login.xml`: the Linden Lab panel (`llpanellogin.cpp`)

Each skin can override these files. `ansastorm`, `firestorm` and `starlight` already have copies.

## Loading Screen (Progress View)

Code: `indra/newview/llprogressview.cpp`. XUI: `panel_progress.xml`, `panel_progress_mini.xml`.

### Layout

The progress bar, the status text (`progress_text`), the logo strip (`logos_lbl`, `panel_icons`) and the spacing all come from XUI.

### Background image

- `LLProgressView::initStartTexture()` (line ~551) loads the user's own snapshot from the per-user folder:
  - `screen_last.png` when logging in to the last location
  - `screen_home.png` (falling back to the legacy `screen_home.bmp`) when logging in at home
- If the file is missing, or `UseStartScreen` is false, the background is black (`LLViewerTexture::sBlackImagep`).
- Customization ideas:
  - A branded fallback image instead of black.
  - Rotating or random AyaneStorm artwork.
- Either needs a small edit in `initStartTexture` wrapped in `AS:Chanayane` tags, or better, a new `as*` module that picks the image path.

### Logos

- `LLProgressView::initLogos()` (line ~488) draws the third-party logos from `skins/default/textures/3p_icons` next to the `logos_lbl` label.
- An AyaneStorm logo could be added there.
- This needs a tagged edit in `llprogressview.cpp`.

### Status text

Startup messages such as "Connecting to region..." come from `strings.xml`, per language. Rewording them is XUI only.

### First-login intro page

- `LLProgressView::revealIntroPanel()` (line ~204) opens `PostFirstLoginIntroURL` in the `login_media_panel` web control on the user's first login.
- It is gated by `PostFirstLoginIntroViewed` and `BrowserJavascriptEnabled`.
- It could point at an AyaneStorm welcome or features page. This is a settings change only.

### Disabling the screens

The existing settings `FSDisableLoginScreens` and `FSDisableLogoutScreens` turn the screens off. They are exposed in `panel_preferences_firestorm.xml`.

## Skins and Themes

- Skins live in `indra/newview/skins/<folder>` and are registered in `skins/skins.xml`. Existing skins: `ansastorm`, `ansastorm_modern`, `default`, `firestorm`, `metaharper`, `starlight`, `starlightcui`, `vintage`.
- A dedicated `ayanestorm` skin can override colors (`colors.xml`), textures and any XUI file (login panel, progress panel, floaters) without touching C++.
- A skin only adds files, so it is the most merge-safe option.
- The default skin and theme come from the `SkinCurrent` and `SkinCurrentTheme` settings.

## General Branding

- **App name and strings:** `APP_NAME` and related entries in `strings.xml`.
- **Window, taskbar and installer icons:** `.ico` and `.png` files in `indra/newview/res` and `indra/newview/icons/`, plus the NSIS installer images.
- **Sounds:** the login and UI sounds can be replaced.

## Effort and Merge-Safety Summary

| Change | Where | C++ needed | Merge risk |
|---|---|---|---|
| Login web page content | `storm.ayane.li` server | No | None |
| Splash options and query parameters | `fspanellogin.cpp`, splash floater XUI | Optional | Low |
| Login panel layout | `panel_fs_login.xml` / `panel_fs_nui_login.xml` in a skin | No | Low |
| Loading screen layout and text | `panel_progress.xml`, `strings.xml` | No | Low |
| First-login intro page | `PostFirstLoginIntroURL` setting | No | None |
| Branded loading background fallback | `initStartTexture` or a new `as*` module | Yes | Low (small tagged edit) |
| Extra loading-screen logo | `initLogos` | Yes | Low (small tagged edit) |
| Full AyaneStorm skin | `skins/ayanestorm`, `skins.xml` | No | Very low |
| App icons, installer, name | `res/`, `icons/`, NSIS, `strings.xml` | No | Low |
