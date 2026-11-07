/**
 * asrtexturequeue.cpp
 * See asrtexturequeue.h.
 */
#include "asrtexturequeue.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace asr
{

uint32_t textureBytesPerPixel(TextureFormat format)
{
    return format == TextureFormat::R8 ? 1 : format == TextureFormat::RG8 ? 2 : 4;
}

uint64_t textureBytes(const TextureDesc& desc)
{
    uint64_t total = 0;
    uint64_t width = desc.width;
    uint64_t height = desc.height;
    for (uint32_t level = 0; level < desc.mipLevels; ++level)
    {
        total += width * height * textureBytesPerPixel(desc.format);
        width = std::max<uint64_t>(width / 2, 1);
        height = std::max<uint64_t>(height / 2, 1);
    }
    return total;
}

TextureQueue::TextureQueue(MemoryBudget* budget) : mBudget(budget)
{
    const uint32_t white = create(TextureDesc());
    TextureCommand fill;
    fill.type = TextureCommand::Type::Upload;
    fill.slot = white;
    fill.width = 1;
    fill.height = 1;
    fill.pixels.assign(4, 0xFF);
    mCommands.push_back(std::move(fill));
}

uint32_t TextureQueue::create(const TextureDesc& desc, uint32_t sourceSlot, uint32_t sourceMip)
{
    TextureCommand command;
    command.type = TextureCommand::Type::Create;
    command.desc = desc;
    command.sourceSlot = sourceSlot;
    command.sourceMip = sourceMip;

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeSlots.empty())
    {
        command.slot = mFreeSlots.back();
        mFreeSlots.pop_back();
    }
    else if (mNextSlot < MAX_TEXTURES)
    {
        command.slot = mNextSlot++;
    }
    else
    {
        return 0;
    }
    if (mBudget)
    {
        mBudget->reserveAnyway(MemoryCategory::Textures, textureBytes(desc));
    }
    const uint32_t slot = command.slot;
    mCommands.push_back(std::move(command));
    return slot;
}

void TextureQueue::upload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          const void* pixels, size_t bytes)
{
    if (slot == 0)
    {
        return;
    }
    std::vector<uint8_t> copy(bytes);
    memcpy(copy.data(), pixels, bytes);
    upload(slot, level, x, y, width, height, std::move(copy));
}

void TextureQueue::upload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          std::vector<uint8_t>&& pixels)
{
    if (slot == 0)
    {
        return;
    }
    TextureCommand command;
    command.type = TextureCommand::Type::Upload;
    command.slot = slot;
    command.level = level;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.pixels = std::move(pixels);

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.push_back(std::move(command));
}

void TextureQueue::generateMips(uint32_t slot)
{
    if (slot == 0)
    {
        return;
    }
    TextureCommand command;
    command.type = TextureCommand::Type::GenerateMips;
    command.slot = slot;

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.push_back(std::move(command));
}

void TextureQueue::destroy(uint32_t slot, uint64_t lastList)
{
    if (slot == 0)
    {
        return;
    }
    TextureCommand command;
    command.type = TextureCommand::Type::Destroy;
    command.slot = slot;
    command.lastList = lastList;

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.push_back(std::move(command));
}

void TextureQueue::take(std::vector<TextureCommand>& commands)
{
    std::lock_guard<std::mutex> lock(mMutex);
    commands.insert(commands.end(), std::make_move_iterator(mCommands.begin()), std::make_move_iterator(mCommands.end()));
    mCommands.clear();
}

void TextureQueue::releaseSlot(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeSlots.push_back(slot);
}

} // namespace asr
