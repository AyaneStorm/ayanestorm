# AyaneStorm source license audit

Date: 2026-11-16

## Scope and results

Checked all repository files matching `as*.cpp`, `as*.h`, and `as*.mm`, including hidden paths.

- 167 AyaneStorm source files checked.
- 149 missing license headers added: 46 in `indra/asrenderer` (including tests and tools), 4 in `indra/llrender`, and 99 in `indra/newview`.
- 18 existing LGPL headers preserved.
- Two upstream files, `indra/newview/gltf/asset.cpp` and `asset.h`, excluded from AyaneStorm ownership; their Linden Research license headers were preserved.

## License header

Model: `indra/newview/fsfloateravataralign.cpp`. Added headers retain its GNU Lesser General Public License version 2.1-only terms and copyright `Copyright (C) 2026, Ayane Lyla`, with `AyaneStorm Viewer Source Code`, author `chanayane@ayanestorm`, and the `viewerlgpl` marker used by existing AyaneStorm headers. Firestorm's organization address and website were omitted.

Existing file descriptions and third-party credits were preserved. Already licensed files were left unchanged, including their existing attribution.

## Verification

Verified every AyaneStorm file has an initial LGPL 2.1-only header with a closing LicenseInfo marker. Each edited file was read back and verified against the intended content; everything after its initial comment was preserved, apart from CRLF-to-LF normalization. Edited files use UTF-8 without BOM and LF line endings. No build was run; changes affect comments only.
