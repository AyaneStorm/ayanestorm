# Texture compression (BC1, BC4, BC5, BC7)

Why: at 84 avatars in a club, textures took 4.6 GB against a 2.9 GB limit, the process
reached the driver's budget, memory spilled into system RAM and the frame rate fell to 10.
Uncompressed RGBA takes 32 bits a texel; BC7 takes 8 and BC1 4.

Decided by the user (2026-11-12):

- BC7 for colour with alpha, and for colour where BC1 would show banding.
- BC1 for other opaque colour.
- BC5 for normal maps (when the renderer draws them) and two-channel images.
- BC4 for single-channel images.
- Encoded the fastest way, at the best quality the texture's purpose needs.
- Encoded results kept in a disk cache, so the work is not done again.

BC formats are sampled in hardware on every target (NVIDIA, AMD, Intel, Apple Silicon).
ETC2 and ASTC are not sampled by desktop NVIDIA and AMD; BC6H is for HDR images.

## Status (2026-11-12)

Step 1, GPU encoding, is written for Vulkan and Metal; the viewer build is untested.
`asrenderer_test --encode` passes on Windows (RTX 3080 Ti) and the Mac Mini M4, with
quality judged by a CPU decoder that matches the hardware exactly for BC7 (BC4/BC5 by 1,
NVIDIA's BC1 thirds by 4):

| image | format | PSNR |
|---|---|---|
| smooth gradient | BC7 | 53.6 dB |
| alpha edges | BC7 | 53.5 dB |
| grainy detail (+-40) | BC1 | 29.7 dB, near BC1's best for that grain |
| single channel | BC4 | 54 to 57 dB |
| two channels | BC5 | 51.2 dB |

Speed on the 3080 Ti, worst case (noise, every block tries a second mode): BC1 about
0.5 ms and BC7 about 3 ms a 1024 texture with mips. Register pressure was the cost: with
texels unpacked into arrays and indices in arrays, BC7 took 26 ms. Per frame at most
`ENCODE_TEXELS_PER_FRAME` (1.5 Mtexels, one 1024 texture) is encoded; a budget from
measured GPU time would suit slow GPUs better, and is left for after the viewer test.

The setting `ASTextureCompression` (default on) turns it off for textures loaded after.
Step 2, the disk cache, is next.

## Which textures

Only images the viewer fetched (`LLViewerFetchedTexture::createTexture()`), and only when
`LLImageGL::mAllowCompression` is set: the viewer already clears it for UI images, media
and sculpt maps (whose RGB are coordinates). UI, fonts, media, render targets and anything
updated by sub-image stay uncompressed. Images under 8 texels across stay uncompressed.

## Choice of format (on the main thread, from the decoded pixels)

`asr::chooseTextureFormat()` (asrenderer, testable):

- one channel (luminance, alpha, red): BC4, with the existing swizzle;
- two channels (luminance-alpha, RG): BC5, with the existing swizzle;
- four channels with any alpha under 255: BC7;
- opaque colour: samples up to 1024 4x4 blocks; a block is smooth when its largest channel
  range is above 0 and at most 24 (BC1's 5-6-5 endpoints and four steps band there). Too
  many smooth blocks: BC7, otherwise BC1.

## Encoding (GPU, both backends, one GLSL source)

Buffers only, by GPU address in push constants, so Vulkan and Metal share everything and
no descriptors or image views are added:

1. Create: the BC image, a transient source buffer (RGBA8, every mip) and a transient
   block buffer. The image is cleared through the block buffer (transparent blocks), as an
   uncompressed image is cleared today, since a list can draw it before its pixels.
2. Upload: rows are copied from the upload ring into the source buffer (RGBA8; one- and
   two-channel images are widened to four bytes on the main thread).
3. Encode, queued after the last piece of an upload: `asrTextureMipC` makes each mip from
   the one before it (a 2x2 box, as the linear blit did), then `asrEncodeBC1C`,
   `asrEncodeBC45C` or `asrEncodeBC7C` writes every mip's blocks, one thread a block, and
   the blocks are copied into the image. The transient buffers go once the frame is done.
4. Shrinking copies BC mips image to image, as today. Growing is a new upload and encode.

Encoders:

- BC1: principal axis, endpoints on it, one least-squares refinement, 4-colour mode.
- BC4/BC5: per-channel min and max, 8-value mode, nearest index.
- BC7: mode 6 (one subset, RGBA) always; for opaque blocks mode 1 (two subsets) over the
  64 partitions ranked by a quick fit, the best one refined; for blocks with alpha mode 5
  (separate alpha). The lowest error wins.

Per frame, encoding is limited by source texels, like uploads are by time, so a teleport
does not stall the frame. Devices without BC (none of the targets) keep uncompressed.

Memory: the BC image counts as Textures; the transient buffers count as Staging, anyway,
and live a few frames.

## Disk cache (second step)

Agreed with the user (2026-11-12):

- Its own folder beside the viewer's texture cache, one file a texture named by its
  UUID, in 16 subfolders by the UUID's first digit, as the texture cache does.
- A file: a small header (magic, encoder version, format, width, height, mips), then the
  mips smallest first, so a smaller size reads only the start of the file (what the
  viewer's split of headers and bodies gives, in one file).
- An index file (UUID, bytes, largest size, last use), so startup lists nothing.
- Size-limited, least recently used first; written to a temporary file and renamed, so a
  crash leaves no half entry; cleared with the viewer's cache.
- Holds the largest encoded size with all its mips. A hit at the wanted size skips the
  JPEG2000 decode as well as the encode. Written on a worker thread from a GPU readback
  of the block buffer.
- Never cached: textures whose UUID stays while their pixels change, as local textures
  (`FTT_LOCAL_FILE`). A new encoder version misses, so old entries are encoded again.

(The viewer's own split of a header file and body files is for speed: many textures
need only their header or first levels, read from one file.)

### Built so far (2026-11-12)

Renderer side, tested on Windows and the Mac (`asrenderer_test --encode`):
`TextureQueue::generateMips(slot, token)` reads an encode's blocks back
(`takeEncoded()`), and `createEncoded()`/`replaceEncoded()` + `uploadBlocks()` +
`generateMips()` make a texture from stored blocks with no encode; the round trip is
byte-identical at every mip.

### Viewer side, written (2026-11-12, unbuilt)

`newview/asrtexturecache.cpp` does all of it; the viewer's files get tagged hooks only:
`LLViewerFetchedTexture::updateFetch()` asks `ASRTextureCache::load()` before the fetcher
(the short circuit), `createTexture()` opens `ASR::CompressionScope(this)`,
`LLTextureCache::purgeAllTextures()` calls `clear()`, and `LLImageGL` and
`LLViewerFetchedTexture` befriend `ASRTextureCacheAccess`. A hit calls the texture's own
`createGLTexture()` with a placeholder image while `ASR::EncodedImageScope` hands the
renderer the blocks, so the name swap and bookkeeping stay the viewer's. The log gives
hits, misses, writes and size once a minute.

### Viewer side, plan

1. `asrtexturecache.cpp` (newview, AS-owned): files, index, size limit
   (`ASTextureCacheMB`), worker-thread reads and writes, cleared with the viewer's cache.
2. Writing: `ASR::CompressionScope` carries the texture's UUID, full size and whether it
   may be cached; the link reads the encode back and the cache writes it, with what the
   viewer computes from pixels at creation: `LLImageGL::mIsMask` and the pick mask
   (`updatePickMask()`, a bit for every second texel, alpha over 32), from the largest
   size, thinned for smaller ones.
3. Reading: in `LLViewerFetchedTexture::updateFetch()`, before a fetch is requested, a
   cached size at least the one wanted is read on a worker thread instead; the texture is
   then made from its blocks through a new `LLImageGL` entry that sets size, discard level,
   mask and pick mask as `createGLTexture()` would. A miss fetches as today.
4. Not cached: textures that need their pixels on the CPU (aux data, load callbacks,
   saved raw images, bakes being composited), local textures, sculpt maps (never
   compressed).

## Verification

- `asrenderer_test --selftest`: block sizes, format choice, and each encoder against
  synthetic images (gradients, noise, alpha edges, normals) decoded back, with quality
  thresholds; encode speed in texels a second, on Windows and the Mac.
- Viewer, same club: texture MB at the same detail against today's 4.6 GB, frame rate,
  and no visible banding on skin and skies.
