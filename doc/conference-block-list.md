# Conference block list

## Behavior

AyaneStorm identifies an ad-hoc conference by its session UUID. The conference toolbar's **Block conference** button stores that UUID and display name in the per-account `ASBlockedConferences` setting, opens the block-list floater, and leaves the session.

Later text, immediate, and voice invitations for a stored UUID are declined before a local conversation session or invitation UI is created. The list remains available at **Comm > Blocked Conferences...**. Selecting an entry and clicking **Unblock** removes it.

## Limitation

The viewer cannot know a new conference UUID before receiving its first invitation. Blocking therefore prevents later invitations or re-adds to the same conference; it cannot predict a conference that has never been seen.

## Implementation points

- `asconferenceblocklist.*`: persistence and block-list floater.
- `fsfloaterim.cpp` and `floater_fs_im_session.xml`: conference-only toolbar action.
- `llimview.cpp`: early rejection in both incoming conference paths.
- `settings_per_account.xml`: persistent LLSD array of `{id, name}` maps.
