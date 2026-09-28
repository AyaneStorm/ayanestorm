# AyaneStorm Japanese Fonts (CJK tofu / missing glyph squares)

## Problem
Users reported Japanese characters rendering as squares (tofu).

## How font loading works (Firestorm)
- `LLFontGL::initClass` (`indra/llrender/llfontgl.cpp`) loads ONE file: the `FSFontSettingsFile` setting (`fonts.xml` = "Inter", or `fonts_<name>.xml`) from `indra/newview/fonts/` (install `fonts/`), then `user_settings/fonts/`.
- Skin `xui/*/fonts.xml` (including `xui/ja/fonts.xml`) is used ONLY if that file is missing. So `ja/fonts.xml` (NotoSansCJKjp-*.otf, YuGoth*) is effectively dead; NotoSansCJKjp is not shipped anyway.
- `<font name="default">` files are the global fallback chain for every font.
- Windows fallback chain was: meiryo.TTC, MSGOTHIC.TTC, gulim, simhei, ArialUni, msyh, Cambria, malgun, micross (searched in the system fonts dir only).
- Meiryo / MS Gothic are part of the optional "Japanese Supplemental Fonts" feature on non-Japanese Windows 10/11, so they can be absent -> tofu.
- Font file buffers are shared through `gFontManagerp->loadFont`, so a fallback file is read into memory once regardless of how many sizes/styles use it.
- Font preset combo: `LLFloaterPreference::loadFontPresetsFromDir` lists every `fonts_*.xml` automatically (`fonts_noto_sans_jp.xml` -> "Noto sans jp").
- Packaging: `viewer_manifest.py` copies `fonts/*.ttf`, `*.txt`, `*.xml` (not `*.otf`).

## AYAstorm approach (`.am/`)
- No JP-specific code; its llfont* diffs are just an older Firestorm base.
- Bundles JP fonts in `indra/newview/fonts/`: NotoSansJP (ttf, OFL), IBMPlexSansJP, LINESeedJP, AlibabaSansJP (otf).
- One font-set XML per font (`fonts_noto_sans_jp.xml`, ...); adds `*.otf` to `viewer_manifest.py`.
- Does NOT add JP fonts to the default fallback chain: only users who pick a JP set benefit.

## AyaneStorm implementation
- Bundled `NotoSansJP-Regular.ttf` / `NotoSansJP-Bold.ttf` (copied from AYAstorm; OFL, covered by `NotoLICENSE_OFL.txt`).
- New set `fonts_noto_sans_jp.xml` (based on `fonts_noto.xml`; NotoSansCombined kept as fallback).
- Every existing set's `default` font gets `NotoSansJP-Regular.ttf` after the emoji fonts and before the `<os>` system fallbacks (tagged with `<AS:Chanayane>` XML comments). Bundled glyphs now win over Meiryo/MS Gothic on all systems.
- IBM Plex / LINE Seed / Alibaba sets not ported (Alibaba license not OFL; no license files in AYAstorm).
