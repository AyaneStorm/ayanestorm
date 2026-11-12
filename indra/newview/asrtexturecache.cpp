/**
 * @file asrtexturecache.cpp
 * @brief See asrtexturecache.h and doc/ayanestorm-new-renderer-texture-compression.md.
 *
 * On disk, under the cache folder's asrtextures: one file a texture, named by its UUID, in
 * 16 folders by its first digit, and index.bin. A file is a header, the pick mask, then
 * the mips smallest first, so a smaller size reads only the start of the file. Files are
 * written to a temporary name and renamed. Without its index the folder is emptied: its
 * files could not be counted against the size limit.
 * Author: chanayane@firestorm
 */
#include "llviewerprecompiledheaders.h"

#include "asrtexturecache.h"

#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN) // as ASR_VIEWER_SUPPORTED in asrviewer.cpp

#include "asrbridge.h"
#include "asrdevice.h"
#include "asrgltable.h"
#include "asrviewer.h"
#include "lldir.h"
#include "llfile.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llthread.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// The few members the cache reads and sets: LLImageGL's and LLViewerFetchedTexture's
// friend, so neither class gains anything else.
class ASRTextureCacheAccess
{
public:
    // Its pixels are wanted on the CPU (callbacks, saved raw images): it is fetched as usual.
    static bool needsPixels(LLViewerFetchedTexture* texture)
    {
        return !texture->mLoadedCallbackList.empty() || texture->mForceToSaveRawImage || texture->mSaveRawImage;
    }

    static bool allowsCompression(const LLImageGL* image)
    {
        return image->mAllowCompression;
    }

    static bool isMask(const LLImageGL* image)
    {
        return image->mIsMask;
    }

    // LLImageGL's pick mask as it is: a bit for every second texel each way, row by row.
    static std::vector<U8> pickMask(const LLImageGL* image)
    {
        if (!image->mPickMask)
        {
            return {};
        }
        const size_t bits = size_t(image->mPickMaskWidth + 1) * size_t(image->mPickMaskHeight + 1);
        return std::vector<U8>(image->mPickMask, image->mPickMask + (bits + 7) / 8);
    }

    // Made as createTexture() would make it, from blocks instead of pixels: the texture's
    // own path, name swap and bookkeeping included.
    static bool create(LLViewerFetchedTexture* texture, S32 discard, U32 fullWidth, U32 fullHeight, S8 components, const ASR::EncodedImage& encoded,
                       U16 width, U16 height, bool mask, const std::vector<U8>& pick)
    {
        LLImageGL* image = texture->getGLTexture();
        if (!image)
        {
            return false;
        }
        // Its pixels are never read: EncodedImageScope gives the renderer the blocks, and
        // keeps LLImageGL from analyzing them.
        LLPointer<LLImageRaw> placeholder = new LLImageRaw(width, height, components);
        if (placeholder->isBufferInvalid())
        {
            return false;
        }
        texture->mFullWidth = fullWidth;
        texture->mFullHeight = fullHeight;
        texture->mOrigWidth = fullWidth;
        texture->mOrigHeight = fullHeight;
        texture->mComponents = components;
        texture->setTexelsPerImage();
        bool made = false;
        {
            ASR::EncodedImageScope scope(encoded);
            made = image->createGLTexture(discard, placeholder, 0, true, texture->getBoostLevel());
        }
        if (!made)
        {
            return false;
        }
        ++image->mAlphaAnalysisSerial; // no analysis started before may overwrite this
        image->mIsMask = mask;
        const size_t bits = size_t(width / 2 + 1) * size_t(height / 2 + 1);
        if (!pick.empty() && pick.size() == (bits + 7) / 8)
        {
            image->createPickMask(width, height);
            memcpy(image->mPickMask, pick.data(), pick.size());
        }
        else
        {
            image->freePickMask();
        }
        texture->mRawDiscardLevel = discard;
        texture->setActive();
        return true;
    }
};

namespace
{
constexpr char MAGIC[4] = { 'A', 'S', 'B', 'C' };
// Raise when the encoders or the file change: older entries then miss and are encoded again.
constexpr U32 ENCODER_VERSION = 1;
constexpr U32 INDEX_VERSION = 1;

#pragma pack(push, 1)
struct FileHeader
{
    char magic[4];
    U32 version;
    U8 format;      // asr::TextureFormat, compressed
    U8 components;
    U8 mipLevels;
    U8 mask;        // LLImageGL::mIsMask
    U16 width;      // the largest mip kept
    U16 height;
    U16 fullWidth;  // the texture's own size
    U16 fullHeight;
    U32 pickBytes;  // the pick mask, for width and height, after the header
};

struct IndexEntry
{
    U8 id[16];
    U16 width;
    U16 height;
    U16 fullWidth;
    U16 fullHeight;
    U32 bytes;
    U32 lastUse;    // seconds since 1970
};
#pragma pack(pop)

struct Entry
{
    U16 width = 0;
    U16 height = 0;
    U16 fullWidth = 0;
    U16 fullHeight = 0;
    U32 bytes = 0;
    U32 lastUse = 0;
};

// A texture's encode waiting to be read back.
struct Storing
{
    LLPointer<LLViewerFetchedTexture> texture;
    S32 discard = 0;
    U32 fullWidth = 0;
    U32 fullHeight = 0;
    S8 components = 0;
    F64 since = 0.0;
};

// A read finished on the worker. Only the main thread holds the texture (sLoading):
// texture reference counts are not for other threads.
struct Read
{
    LLViewerFetchedTexture* texture = nullptr;
    S32 discard = 0;
    bool ok = false;
    FileHeader header{};
    U16 width = 0;          // the mip read as level 0
    U16 height = 0;
    asr::TextureDesc desc;
    std::vector<U8> blocks; // largest mip first
    std::vector<U64> offsets;
    std::vector<U8> pick;   // thinned to width and height
};

bool sStarted = false;
std::string sFolder;
std::unordered_map<LLUUID, Entry> sEntries;
U64 sTotalBytes = 0;
std::unordered_map<U64, Storing> sStoring;
U64 sNextToken = 0;
std::unordered_map<LLViewerFetchedTexture*, LLPointer<LLViewerFetchedTexture>> sLoading;
U64 sHits = 0;
U64 sMisses = 0;
U64 sWrites = 0;
U64 sFailures = 0;
F64 sLoggedAt = 0.0;

// One worker thread does every file operation, in order.
std::thread sWorker;
std::mutex sMutex;
std::condition_variable sWake;
std::condition_variable sIdle;
std::deque<std::function<void()>> sJobs;
std::deque<Read> sReads;
bool sStopping = false;
bool sBusy = false;

U32 now()
{
    return U32(std::time(nullptr));
}

std::string folderOf(const LLUUID& id)
{
    return sFolder + gDirUtilp->getDirDelimiter() + id.asString().substr(0, 1);
}

std::string pathOf(const LLUUID& id)
{
    return folderOf(id) + gDirUtilp->getDirDelimiter() + id.asString() + ".asbc";
}

std::string indexPath()
{
    return sFolder + gDirUtilp->getDirDelimiter() + "index.bin";
}

std::string folderPath()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "asrtextures");
}

U64 limitBytes()
{
    static LLCachedControl<U32> limit(gSavedSettings, "ASTextureCacheMB", 4096);
    return U64(U32(limit)) * 1024 * 1024;
}

void post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sJobs.push_back(std::move(job));
    }
    sWake.notify_one();
}

void work()
{
    std::unique_lock<std::mutex> lock(sMutex);
    while (true)
    {
        sWake.wait(lock, [] { return sStopping || !sJobs.empty(); });
        if (sJobs.empty())
        {
            return; // stopping, and every job done
        }
        std::function<void()> job = std::move(sJobs.front());
        sJobs.pop_front();
        sBusy = true;
        lock.unlock();
        job();
        lock.lock();
        sBusy = false;
        if (sJobs.empty())
        {
            sIdle.notify_all();
        }
    }
}

void waitIdle()
{
    std::unique_lock<std::mutex> lock(sMutex);
    sIdle.wait(lock, [] { return sJobs.empty() && !sBusy; });
}

S32 discardOf(U32 fullWidth, U32 width)
{
    S32 discard = 0;
    while (width != 0 && (width << discard) < fullWidth)
    {
        ++discard;
    }
    return discard;
}

asr::TextureDesc descOf(U8 format, U16 width, U16 height, U8 mipLevels)
{
    asr::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = mipLevels;
    desc.format = asr::TextureFormat(format);
    return desc;
}

U32 mipCount(U32 width, U32 height)
{
    U32 count = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
        ++count;
    }
    return count;
}

void removeEntry(const LLUUID& id)
{
    const auto found = sEntries.find(id);
    if (found == sEntries.end())
    {
        return;
    }
    sTotalBytes -= std::min<U64>(sTotalBytes, found->second.bytes);
    sEntries.erase(found);
    const std::string path = pathOf(id);
    post([path]() { LLFile::remove(path, ENOENT); });
}

// Least recently used first, down to 90% of the limit.
void trim()
{
    const U64 limit = limitBytes();
    if (sTotalBytes <= limit)
    {
        return;
    }
    std::vector<std::pair<U32, LLUUID>> byAge;
    byAge.reserve(sEntries.size());
    for (const auto& [id, entry] : sEntries)
    {
        byAge.emplace_back(entry.lastUse, id);
    }
    std::sort(byAge.begin(), byAge.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [age, id] : byAge)
    {
        if (sTotalBytes <= limit / 10 * 9)
        {
            break;
        }
        removeEntry(id);
    }
}

void saveIndex()
{
    if (sFolder.empty())
    {
        return;
    }
    const std::string path = indexPath();
    const std::string temporary = path + ".tmp";
    {
        llofstream file(temporary.c_str(), std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return;
        }
        const U32 header[3] = { INDEX_VERSION, ENCODER_VERSION, U32(sEntries.size()) };
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& [id, entry] : sEntries)
        {
            IndexEntry stored{};
            memcpy(stored.id, id.mData, 16);
            stored.width = entry.width;
            stored.height = entry.height;
            stored.fullWidth = entry.fullWidth;
            stored.fullHeight = entry.fullHeight;
            stored.bytes = entry.bytes;
            stored.lastUse = entry.lastUse;
            file.write(reinterpret_cast<const char*>(&stored), sizeof(stored));
        }
    }
    LLFile::remove(path, ENOENT);
    LLFile::rename(temporary, path);
}

bool loadIndex()
{
    llifstream file(indexPath().c_str(), std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    U32 header[3] = {};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != INDEX_VERSION || header[1] != ENCODER_VERSION)
    {
        return false;
    }
    for (U32 i = 0; i < header[2]; ++i)
    {
        IndexEntry stored{};
        if (!file.read(reinterpret_cast<char*>(&stored), sizeof(stored)))
        {
            return false;
        }
        LLUUID id;
        memcpy(id.mData, stored.id, 16);
        Entry& entry = sEntries[id];
        entry.width = stored.width;
        entry.height = stored.height;
        entry.fullWidth = stored.fullWidth;
        entry.fullHeight = stored.fullHeight;
        entry.bytes = stored.bytes;
        entry.lastUse = stored.lastUse;
        sTotalBytes += stored.bytes;
    }
    return true;
}

// Worker: the mips from skip down, the pick mask thinned to their size.
Read readFile(const std::string& path, LLViewerFetchedTexture* texture, S32 discard, U32 skip)
{
    Read read;
    read.texture = texture;
    read.discard = discard;
    llifstream file(path.c_str(), std::ios::binary);
    FileHeader& header = read.header;
    if (!file.is_open() || !file.read(reinterpret_cast<char*>(&header), sizeof(header)) || memcmp(header.magic, MAGIC, 4) != 0 ||
        header.version != ENCODER_VERSION || !asr::textureCompressed(asr::TextureFormat(header.format)) || skip >= header.mipLevels ||
        header.mipLevels != mipCount(header.width, header.height))
    {
        return read;
    }
    std::vector<U8> pick(header.pickBytes);
    if (!pick.empty() && !file.read(reinterpret_cast<char*>(pick.data()), std::streamsize(pick.size())))
    {
        return read;
    }
    read.width = U16(std::max(header.width >> skip, 1));
    read.height = U16(std::max(header.height >> skip, 1));
    read.desc = descOf(header.format, read.width, read.height, U8(header.mipLevels - skip));
    // Smallest first on disk: the mips wanted are the first bytes.
    const U64 wanted = asr::textureBytes(read.desc);
    std::vector<U8> smallestFirst(static_cast<size_t>(wanted));
    if (!file.read(reinterpret_cast<char*>(smallestFirst.data()), std::streamsize(wanted)))
    {
        return read;
    }
    read.blocks.resize(size_t(wanted));
    read.offsets.resize(read.desc.mipLevels);
    U64 at = 0;
    for (S32 level = S32(read.desc.mipLevels) - 1; level >= 0; --level)
    {
        const U64 offset = asr::textureMipOffset(read.desc, U32(level), true);
        const U64 bytes = asr::textureMipOffset(read.desc, U32(level) + 1, true) - offset;
        memcpy(read.blocks.data() + offset, smallestFirst.data() + at, size_t(bytes));
        read.offsets[level] = offset;
        at += bytes;
    }

    // LLImageGL's pick mask: a bit for every second texel each way, row by row; thinned by
    // taking every 2^skip-th bit of the larger one.
    if (!pick.empty())
    {
        const U32 storedAcross = (header.width + 1) / 2;
        const U32 storedRows = (header.height + 1) / 2;
        if (pick.size() == (size_t(header.width / 2 + 1) * size_t(header.height / 2 + 1) + 7) / 8)
        {
            const size_t bits = size_t(read.width / 2 + 1) * size_t(read.height / 2 + 1);
            read.pick.assign((bits + 7) / 8, 0);
            U32 bit = 0;
            for (U32 y = 0; y < read.height; y += 2)
            {
                for (U32 x = 0; x < read.width; x += 2, ++bit)
                {
                    const U32 sx = std::min((x << skip) / 2, storedAcross - 1);
                    const U32 sy = std::min((y << skip) / 2, storedRows - 1);
                    const U32 source = sy * storedAcross + sx;
                    if (pick[source / 8] & (1 << (source % 8)))
                    {
                        read.pick[bit / 8] |= U8(1 << (bit % 8));
                    }
                }
            }
        }
    }
    read.ok = true;
    return read;
}

bool cacheable(LLViewerFetchedTexture* texture)
{
    if (!sStarted || !texture || !ASR::active())
    {
        return false;
    }
    static LLCachedControl<bool> compress(gSavedSettings, "ASTextureCompression", true);
    if (!compress || !ASRViewer::device()->textures().compressionSupported())
    {
        return false;
    }
    // Local textures keep their UUID while their file changes; previews, icons and
    // thumbnails are scaled on the CPU.
    const FTType type = texture->getFTType();
    const S32 boost = texture->getBoostLevel();
    LLImageGL* image = texture->getGLTexture();
    return (type == FTT_DEFAULT || type == FTT_SERVER_BAKE) && image && ASRTextureCacheAccess::allowsCompression(image) && !texture->needsAux() &&
           !ASRTextureCacheAccess::needsPixels(texture) && boost != LLGLTexture::BOOST_ICON && boost != LLGLTexture::BOOST_THUMBNAIL &&
           boost != LLGLTexture::BOOST_PREVIEW && texture->getUseMipMaps();
}

// Main thread: an encode read back becomes a file, if the texture still shows that image.
void write(const asr::TextureQueue::EncodedTexture& encoded, const Storing& storing)
{
    LLViewerFetchedTexture* texture = storing.texture.get();
    LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
    const asr::TextureDesc& desc = encoded.desc;
    if (!image || image->getDiscardLevel() != storing.discard || U32(image->getWidth(storing.discard)) != desc.width ||
        U32(image->getHeight(storing.discard)) != desc.height || desc.width > 0xFFFF || desc.height > 0xFFFF ||
        desc.mipLevels != mipCount(desc.width, desc.height) || encoded.blocks.size() != asr::textureBytes(desc))
    {
        return;
    }
    const LLUUID id = texture->getID();
    const auto existing = sEntries.find(id);
    if (existing != sEntries.end() && existing->second.width >= desc.width)
    {
        return; // as sharp already
    }

    FileHeader header{};
    memcpy(header.magic, MAGIC, 4);
    header.version = ENCODER_VERSION;
    header.format = U8(desc.format);
    header.components = U8(storing.components);
    header.mipLevels = U8(desc.mipLevels);
    header.mask = ASRTextureCacheAccess::isMask(image) ? 1 : 0;
    header.width = U16(desc.width);
    header.height = U16(desc.height);
    header.fullWidth = U16(storing.fullWidth);
    header.fullHeight = U16(storing.fullHeight);
    std::vector<U8> pick = ASRTextureCacheAccess::pickMask(image);
    header.pickBytes = U32(pick.size());

    auto bytes = std::make_shared<std::vector<U8>>();
    bytes->reserve(sizeof(header) + pick.size() + encoded.blocks.size());
    bytes->insert(bytes->end(), reinterpret_cast<const U8*>(&header), reinterpret_cast<const U8*>(&header) + sizeof(header));
    bytes->insert(bytes->end(), pick.begin(), pick.end());
    for (S32 level = S32(desc.mipLevels) - 1; level >= 0; --level) // smallest first
    {
        const std::ptrdiff_t offset = std::ptrdiff_t(asr::textureMipOffset(desc, U32(level), true));
        const std::ptrdiff_t end = std::ptrdiff_t(asr::textureMipOffset(desc, U32(level) + 1, true));
        bytes->insert(bytes->end(), encoded.blocks.begin() + offset, encoded.blocks.begin() + end);
    }

    if (existing != sEntries.end())
    {
        sTotalBytes -= std::min<U64>(sTotalBytes, existing->second.bytes);
    }
    Entry& entry = sEntries[id];
    entry.width = U16(desc.width);
    entry.height = U16(desc.height);
    entry.fullWidth = U16(storing.fullWidth);
    entry.fullHeight = U16(storing.fullHeight);
    entry.bytes = U32(bytes->size());
    entry.lastUse = now();
    sTotalBytes += entry.bytes;
    ++sWrites;

    const std::string folder = folderOf(id);
    const std::string path = pathOf(id);
    post([folder, path, bytes]() {
        if (!LLFile::isdir(folder))
        {
            LLFile::mkdir(folder);
        }
        const std::string temporary = path + ".tmp";
        {
            llofstream file(temporary.c_str(), std::ios::binary | std::ios::trunc);
            if (!file.is_open() || !file.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size())))
            {
                return;
            }
        }
        LLFile::remove(path, ENOENT);
        LLFile::rename(temporary, path);
    });
    trim();
}

// Main thread: a read becomes the texture's image.
void make(Read& read)
{
    const auto loading = sLoading.find(read.texture);
    if (loading == sLoading.end())
    {
        return;
    }
    LLPointer<LLViewerFetchedTexture> held = loading->second; // alive until this returns
    LLViewerFetchedTexture* texture = held.get();
    sLoading.erase(loading);
    if (!read.ok)
    {
        ++sFailures;
        removeEntry(texture->getID()); // fetched as usual from now on
        return;
    }
    if (texture->getNumRefs() <= 1)
    {
        return; // only held: nobody else wants it any more
    }
    const S32 current = texture->getDiscardLevel();
    if (current >= 0 && current <= read.discard)
    {
        return; // something as sharp came first
    }
    ASR::EncodedImage encoded;
    encoded.storage = read.header.format;
    encoded.mipLevels = read.desc.mipLevels;
    encoded.blocks = read.blocks.data();
    encoded.mipOffsets = read.offsets.data();
    if (!ASRTextureCacheAccess::create(texture, read.discard, read.header.fullWidth, read.header.fullHeight, S8(read.header.components), encoded,
                                       read.width, read.height, read.header.mask != 0, read.pick))
    {
        ++sFailures;
        return;
    }
    ++sHits;
}
}

void ASRTextureCache::start()
{
    if (sStarted)
    {
        return;
    }
    sFolder = folderPath();
    if (!LLFile::isdir(sFolder))
    {
        LLFile::mkdir(sFolder);
    }
    sEntries.clear();
    sTotalBytes = 0;
    if (!loadIndex())
    {
        // Files without an index cannot be counted against the limit: start empty.
        sEntries.clear();
        sTotalBytes = 0;
        gDirUtilp->deleteDirAndContents(sFolder);
        LLFile::mkdir(sFolder);
    }
    else
    {
        // Until saved again, an index read may be out of date: a crash would leave it so.
        LLFile::remove(indexPath(), ENOENT);
    }
    sStopping = false;
    sWorker = std::thread(work);
    sStarted = true;
    trim();
    LL_INFOS("ASRenderer") << "texture cache: " << sEntries.size() << " textures, " << sTotalBytes / (1024 * 1024) << " MB of "
                           << limitBytes() / (1024 * 1024) << " MB, in " << sFolder << LL_ENDL;
}

void ASRTextureCache::stop()
{
    if (!sStarted)
    {
        return;
    }
    sStarted = false;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStopping = true;
    }
    sWake.notify_one();
    if (sWorker.joinable())
    {
        sWorker.join();
    }
    saveIndex();
    sStoring.clear();
    sLoading.clear();
    sReads.clear();
    LL_INFOS("ASRenderer") << "texture cache: " << sHits << " hits, " << sWrites << " written, " << sFailures << " failed; "
                           << sEntries.size() << " textures, " << sTotalBytes / (1024 * 1024) << " MB" << LL_ENDL;
}

void ASRTextureCache::clear()
{
    if (sStarted)
    {
        waitIdle();
    }
    sEntries.clear();
    sTotalBytes = 0;
    const std::string folder = folderPath();
    if (LLFile::isdir(folder))
    {
        gDirUtilp->deleteDirAndContents(folder);
    }
    if (sStarted)
    {
        LLFile::mkdir(folder);
    }
}

bool ASRTextureCache::load(LLViewerFetchedTexture* texture, S32 desiredDiscard)
{
    if (!sStarted || !texture)
    {
        return false;
    }
    if (sLoading.count(texture))
    {
        return true; // being read
    }
    if (!cacheable(texture))
    {
        return false;
    }
    const LLUUID id = texture->getID();
    const auto found = sEntries.find(id);
    if (found == sEntries.end())
    {
        ++sMisses;
        return false;
    }
    Entry& entry = found->second;
    if (texture->getFullWidth() > 0 && (U32(texture->getFullWidth()) != entry.fullWidth || U32(texture->getFullHeight()) != entry.fullHeight))
    {
        removeEntry(id);
        return false;
    }
    // Only when it is as sharp as wanted: a sharper fetch would replace it at once.
    const S32 cachedDiscard = discardOf(entry.fullWidth, entry.width);
    const S32 discard = llmax(desiredDiscard, cachedDiscard);
    if (desiredDiscard < cachedDiscard)
    {
        ++sMisses;
        return false;
    }
    const S32 current = texture->getDiscardLevel();
    if (current >= 0 && current <= discard)
    {
        return false;
    }
    entry.lastUse = now();
    sLoading[texture] = texture; // held here, on the main thread, until make()
    const std::string path = pathOf(id);
    const U32 skip = U32(discard - cachedDiscard);
    post([path, texture, discard, skip]() {
        Read read = readFile(path, texture, discard, skip);
        std::lock_guard<std::mutex> lock(sMutex);
        sReads.push_back(std::move(read));
    });
    return true;
}

U64 ASRTextureCache::store(LLViewerFetchedTexture* texture)
{
    // The cache's state is the main thread's; a texture made on the image thread is not kept.
    if (!on_main_thread() || !cacheable(texture))
    {
        return 0;
    }
    LLImageGL* image = texture->getGLTexture();
    const S32 discard = image->getDiscardLevel();
    if (discard < 0 || texture->getFullWidth() <= 0 || texture->getFullHeight() <= 0)
    {
        return 0;
    }
    const auto existing = sEntries.find(texture->getID());
    if (existing != sEntries.end() && existing->second.width >= U32(image->getWidth(discard)))
    {
        return 0; // as sharp already
    }
    Storing storing;
    storing.texture = texture;
    storing.discard = discard;
    storing.fullWidth = U32(texture->getFullWidth());
    storing.fullHeight = U32(texture->getFullHeight());
    storing.components = S8(image->getComponents());
    storing.since = LLTimer::getTotalSeconds();
    const U64 token = ++sNextToken;
    sStoring[token] = storing;
    return token;
}

void ASRTextureCache::update()
{
    if (!sStarted)
    {
        return;
    }
    std::vector<asr::TextureQueue::EncodedTexture> encoded;
    ASRViewer::device()->textures().takeEncoded(encoded);
    for (const asr::TextureQueue::EncodedTexture& texture : encoded)
    {
        const auto storing = sStoring.find(texture.token);
        if (storing != sStoring.end())
        {
            write(texture, storing->second);
            sStoring.erase(storing);
        }
    }
    // An encode the renderer dropped (its texture replaced first) is never read back.
    const F64 seconds = LLTimer::getTotalSeconds();
    for (auto it = sStoring.begin(); it != sStoring.end();)
    {
        it = seconds - it->second.since > 30.0 ? sStoring.erase(it) : std::next(it);
    }

    std::deque<Read> reads;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        reads.swap(sReads);
    }
    for (Read& read : reads)
    {
        make(read);
    }

    if (seconds - sLoggedAt >= 60.0)
    {
        sLoggedAt = seconds;
        LL_INFOS("ASRenderer") << "texture cache: " << sHits << " hits, " << sMisses << " misses, " << sWrites << " written, " << sFailures
                               << " failed; " << sEntries.size() << " textures, " << sTotalBytes / (1024 * 1024) << " MB" << LL_ENDL;
        saveIndex();
    }
}

#else // the new renderer is not in this build

void ASRTextureCache::start() {}
void ASRTextureCache::stop() {}
void ASRTextureCache::clear() {}
bool ASRTextureCache::load(LLViewerFetchedTexture*, S32) { return false; }
U64 ASRTextureCache::store(LLViewerFetchedTexture*) { return 0; }
void ASRTextureCache::update() {}

#endif
