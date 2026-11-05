/**
 * asrtexturequeue.h
 * How the main thread creates and fills textures without touching a GPU API. A
 * texture's slot exists at once, so a UI list can use it straight away, and the
 * render thread does the real work before it draws that list. Slots are the index
 * into the bindless texture array, so a texture needs no binding at draw time.
 *
 * Slot 0 is a white pixel, and is what an unbound texture means.
 */
#pragma once

#include "asruilist.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace asr
{

enum class TextureFormat : uint8_t
{
    RGBA8,
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

struct TextureCommand
{
    enum class Type : uint8_t
    {
        Create,
        Upload,
        Destroy
    };

    Type type = Type::Create;
    uint32_t slot = 0;
    TextureDesc desc;              // Create
    uint32_t level = 0;            // Upload
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;   // Upload, tightly packed
    uint64_t lastList = 0;         // Destroy: the newest UI list that may still use it
};

class TextureQueue
{
public:
    TextureQueue();

    // Main thread. Returns slot 0 when every slot is taken.
    uint32_t create(const TextureDesc& desc);

    // Main thread. Copies the pixels, so the caller can free them at once.
    void upload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const void* pixels, size_t bytes);

    // Main thread. lastList is the newest list recorded so far, including the one
    // being recorded now: the texture is freed only once no list can draw it.
    void destroy(uint32_t slot, uint64_t lastList);

    // Render thread. Adds what is waiting to commands, in order.
    void take(std::vector<TextureCommand>& commands);

    // Render thread. Gives back the slot of a texture that has been destroyed.
    void releaseSlot(uint32_t slot);

private:
    std::mutex mMutex; // held only to move a command, so neither thread really waits
    std::vector<TextureCommand> mCommands;
    std::vector<uint32_t> mFreeSlots;
    uint32_t mNextSlot = 0;
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
