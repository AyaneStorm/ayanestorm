/**
 * @file asrtexturecache.h
 * @brief The new renderer's disk cache of compressed textures.
 *
 * A texture the GPU compressed (BC1, BC4, BC5, BC7) is kept on disk as its blocks, with
 * what the viewer works out from its pixels (alpha mask, pick mask). Seen again, it is
 * made from those blocks: no JPEG2000 decode and no encode. See
 * doc/ayanestorm-new-renderer-texture-compression.md.
 * Author: chanayane@ayanestorm
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
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
 * $/LicenseInfo$
 */
#ifndef ASR_TEXTURE_CACHE_H
#define ASR_TEXTURE_CACHE_H

#include "stdtypes.h"

class LLViewerFetchedTexture;

namespace ASRTextureCache
{
    // With the renderer: reads the index, and trims to the size limit.
    void start();
    // Writes the index; waits for files being written.
    void stop();
    // With the viewer's texture cache: every entry goes.
    void clear();

    // LLViewerFetchedTexture::updateFetch(), before it asks the fetcher: true when the
    // cache has the texture at the wanted size or sharper, or is reading it, and the fetch
    // is not needed. The texture is made once the read is done, in update().
    bool load(LLViewerFetchedTexture* texture, S32 desiredDiscard);

    // The renderer is about to encode this texture's new image: nonzero is the token to
    // read its blocks back with, which update() then writes.
    U64 store(LLViewerFetchedTexture* texture);

    // Main thread, once a frame: writes what was read back, makes what was read.
    void update();
}

#endif // ASR_TEXTURE_CACHE_H
