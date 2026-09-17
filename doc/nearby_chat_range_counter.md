# Nearby Chat Range Counter

## Scope

The optional counter on the right of the Nearby Chat toolbar shows other avatars inside the range selected by the Whisper/Say/Shout split button. It is disabled by default through `ASShowNearbyChatRangeCounter` in AyaneStorm Preferences > Conversations.

## Reused Viewer Sources

- Simulator-specific ranges come from `LFSimFeatureHandler::whisperRange()`, `sayRange()`, and `shoutRange()`.
- Say and shout colors use the live minimap entries `MapChatRingColor` and `MapShoutRingColor`. Whisper uses the clearer `LtBlue` UI color (`0 0.5 1 1`) because the minimap's pure blue is difficult to read against the gray toolbar. Counter text forces alpha to `1.0` because ring alpha is too dark for text.
- Avatar IDs and corrected distances come from the same prepared `FSRadar` rows used by Nearby People; `gAgentID` is explicitly excluded. This includes the radar's bridge-assisted correction for unknown coarse altitude.
- Nearby-agent RLV restrictions are respected. The counter displays `--` when nearby agents may not be shown.

## Runtime Behavior

The AS-owned XUI control checks the count once per second while visible. A count change starts a 0.6-second expanding colored flash. When the preference is disabled, XUI hides the control and its draw path also exits before all lookup, counting, timer, and animation work.

`ASShowNearbyChatRangeFriendCount` optionally appends the number of friends as `total (friends)`. It is disabled by default, depends on the main counter preference, and grows the counter panel to the measured text width only while needed. Hover text explains both values and notes that the user's avatar is excluded.

The toolbar reserves 44 pixels after the counter while the option is enabled. This keeps the counter immediately left of the Conversations detach/close controls instead of underneath them when Nearby Chat is docked.
