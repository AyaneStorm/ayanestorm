/**
 * asrtexturequeue.h
 * How the main thread creates and fills textures without touching a GPU API. A
 * texture's slot exists at once, so a UI list can use it straight away, and the
 * render thread does the real work before it draws that list. Slots are the index
 * into the bindless texture array, so a texture needs no binding at draw time.
 *
 * Slot 0 is a white pixel, and is what an unbound texture means.
 *
 * Pixels go once, from the main thread straight into the upload ring: memory the
 * device made that the CPU writes and the GPU copies from. The render thread only
 * records those copies. An upload that finds no room waits here, in order with
 * everything after it, until the GPU has finished with enough of the ring.
 */
#pragma once

#include "asrmemorybudget.h"
#include "asruilist.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace asr
{

enum class TextureFormat : uint8_t
{
    RGBA8,
    BGRA8, // web and media pages arrive like this, and need no conversion
    RG8,
    R8
};

// What the shader reads from the stored channels.
enum class TextureSwizzle : uint8_t
{
    Identity,
    AlphaOnly,      // 0, 0, 0, R: GL_ALPHA, used by font glyphs
    Luminance,      // R, R, R, 1: GL_LUMINANCE
    LuminanceAlpha  // R, R, R, G: GL_LUMINANCE_ALPHA
};

struct TextureDesc
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8;
    TextureSwizzle swizzle = TextureSwizzle::Identity;
};

uint32_t textureBytesPerPixel(TextureFormat format);

// The memory a texture takes, with all of its mips.
uint64_t textureBytes(const TextureDesc& desc);

// Where each piece of an upload starts in the ring, which also keeps every copy's
// offset a multiple of the texel size the APIs ask for.
constexpr uint64_t UPLOAD_RING_ALIGNMENT = 16;

// How long one frame may spend on texture commands: creates cost about 10 us each, and
// a teleport asks for thousands. The rest waits for the next frame, in order. A share of
// the frame, so a slow frame rate does not also make textures arrive slowly.
inline double textureCommandSeconds(double frameSeconds)
{
    const double share = frameSeconds * 0.1;
    return share < 0.001 ? 0.001 : share > 0.004 ? 0.004 : share;
}

struct TextureCommand
{
    enum class Type : uint8_t
    {
        Create,
        Upload,
        GenerateMips,
        Destroy
    };

    Type type = Type::Create;
    uint32_t slot = 0;
    TextureDesc desc;              // Create
    uint32_t sourceSlot = 0;       // Create: the texture to take mips from, 0 to start cleared
    uint32_t sourceMip = 0;        // Create: which of its mips becomes level 0 here
    uint32_t level = 0;            // Upload: one piece of an upload, a run of whole rows
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t ringOffset = 0;       // Upload: where its rows are in the ring, tightly packed
    uint64_t ringBytes = 0;
    uint64_t ringEnd = 0;          // Upload: pass to releaseRing() once the GPU has copied it
    uint64_t lastList = 0;         // Destroy: the newest UI list that may still use it
};

class TextureQueue
{
public:
    // Memory is counted here, as the texture is asked for, so the main thread always
    // knows the real figure. With no budget nothing is counted, which is for tests.
    explicit TextureQueue(MemoryBudget* budget = nullptr);

    // Main thread. Returns slot 0 when every slot is taken. Whether a texture should
    // exist is the caller's call: memory is counted even past the limit.
    // With a sourceSlot, the new texture is that one's mips from sourceMip down, copied
    // on the GPU. desc must be the size of that mip, with no more levels than it has left.
    uint32_t create(const TextureDesc& desc, uint32_t sourceSlot = 0, uint32_t sourceMip = 0);

    // Main thread. How many create() calls can succeed now. A destroyed texture's slot
    // comes back only once the frames that may draw it have finished.
    uint32_t freeSlots();

    // Main thread. Where to write an upload's rows, rowBytes each and tightly packed,
    // usually straight into the ring. Write all of them, then call finishUpload(), with
    // no other call to this queue in between. Returns nullptr for slot 0, which still
    // needs its finishUpload().
    uint8_t* beginUpload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t rowBytes);
    void finishUpload();

    // Main thread. Copies the pixels, so the caller can free them at once.
    void upload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const void* pixels, size_t bytes);

    // Main thread. Makes every smaller mip from level 0, on the GPU.
    void generateMips(uint32_t slot);

    // Main thread. lastList is the newest list recorded so far, including the one
    // being recorded now: the texture is freed only once no list can draw it.
    void destroy(uint32_t slot, uint64_t lastList);

    // Main thread, once a loop. Writes uploads that waited for room into the ring,
    // as far as it has room now.
    void flush();

    // Device, as it starts. The ring stays mapped for the queue's life. No piece of an
    // upload is larger than pieceBytes, which is what one frame may copy.
    void setUploadRing(uint8_t* memory, uint64_t bytes, uint64_t pieceBytes);

    // Render thread. Adds what is waiting to commands, in order.
    void take(std::vector<TextureCommand>& commands);

    // Render thread, or a GPU completion handler. The GPU has finished copying every
    // upload up to this ringEnd, so the ring may be written there again.
    void releaseRing(uint64_t ringEnd);

    // Render thread. Gives back the slot of a texture that has been destroyed.
    void releaseSlot(uint32_t slot);

    // Any thread. Bytes of uploads that wait for room in the ring, for the log.
    uint64_t waitingBytes() const { return mWaitingBytes.load(std::memory_order_relaxed); }

private:
    // A command behind an upload that found no room, with that upload's pixels.
    struct Waiting
    {
        TextureCommand command;
        std::vector<uint8_t> pixels;
        uint32_t rowBytes = 0;
        uint32_t rowsDone = 0;
    };

    void queue(TextureCommand&& command); // with mProducer held
    bool reserve(uint64_t bytes, uint64_t& position);
    void sendPieces(const TextureCommand& upload, uint32_t rowBytes, uint32_t firstRow, uint32_t rows, uint64_t position);
    bool writeRows(Waiting& upload);
    void sendWaiting();

    MemoryBudget* mBudget;

    // Held by the thread that queues, including while it writes pixels, so the render
    // thread never waits on it.
    std::mutex mProducer;
    uint8_t* mRing = nullptr;
    uint64_t mRingSize = 0;
    uint64_t mPieceBytes = 0;
    uint64_t mRingHead = 0;                // positions only grow; the place in memory is position % mRingSize
    std::atomic<uint64_t> mRingTail{ 0 };  // where the GPU has finished reading
    std::deque<Waiting> mWaiting;
    std::atomic<uint64_t> mWaitingBytes{ 0 };
    Waiting mOpen;                         // between beginUpload() and finishUpload()
    bool mOpenInRing = false;
    uint64_t mOpenPosition = 0;

    std::mutex mMutex; // held only to move commands, so neither thread really waits
    std::vector<TextureCommand> mCommands;
    std::vector<uint32_t> mFreeSlots;
    uint32_t mNextSlot = 0;
};

// Which slots each frame's own copy of the texture array has still to take. A frame's copy
// changes only as that frame starts, after its previous use has finished, so no frame on
// the GPU ever sees a slot change under it. With one shared array it did: a new texture
// reached frames already committed, for which it was not resident, a GPU page fault on
// Metal, and Vulkan does not allow descriptors in use to change. Render thread only.
class TextureSlotCopies
{
public:
    explicit TextureSlotCopies(uint32_t copies) : mPending(copies), mAll(copies, false) {}

    void changed(uint32_t slot)
    {
        for (std::vector<uint32_t>& pending : mPending)
        {
            pending.push_back(slot);
        }
    }

    void changedAll()
    {
        for (size_t i = 0; i < mPending.size(); ++i)
        {
            mAll[i] = true;
            mPending[i].clear();
        }
    }

    // Calls write(slot) for each slot the copy has to take, or writeAll() once instead.
    template <class Write, class WriteAll>
    void bring(uint32_t copy, Write write, WriteAll writeAll)
    {
        if (mAll[copy])
        {
            writeAll();
            mAll[copy] = false;
        }
        else
        {
            for (uint32_t slot : mPending[copy])
            {
                write(slot);
            }
        }
        mPending[copy].clear();
    }

private:
    std::vector<std::vector<uint32_t>> mPending;
    std::vector<bool> mAll;
};

// Textures on their way out. One stays until no list can draw it, then until the
// frames that may have drawn it have finished. Render thread only.
class RetiredTextures
{
public:
    void add(uint32_t slot, uint64_t lastList) { mTextures.push_back({ slot, lastList, 0 }); }

    // Calls destroy(slot) for each texture that has become safe. newestList is the
    // list being drawn now, and frame counts frames from 0.
    template <class Destroy>
    void run(uint64_t newestList, uint64_t frame, uint32_t framesInFlight, Destroy destroy)
    {
        for (size_t i = 0; i < mTextures.size();)
        {
            Retired& texture = mTextures[i];
            if (texture.safeFrame == 0 && newestList > texture.lastList)
            {
                texture.safeFrame = frame + framesInFlight + 1;
            }
            if (texture.safeFrame != 0 && frame >= texture.safeFrame)
            {
                destroy(texture.slot);
                mTextures[i] = mTextures.back();
                mTextures.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    size_t size() const { return mTextures.size(); }

private:
    struct Retired
    {
        uint32_t slot;
        uint64_t lastList;  // no list newer than this can draw it
        uint64_t safeFrame; // 0 until that list has been passed
    };

    std::vector<Retired> mTextures;
};

} // namespace asr
