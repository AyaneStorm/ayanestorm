/**
 * asrtexturequeue.cpp
 * See asrtexturequeue.h.
 */
#include "asrtexturequeue.h"

#include <algorithm>
#include <cstring>
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
    // Its pixel waits here until the device gives the ring.
    const uint32_t white = create(TextureDesc());
    const uint8_t pixel[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    std::lock_guard<std::mutex> producer(mProducer);
    Waiting fill;
    fill.command.type = TextureCommand::Type::Upload;
    fill.command.slot = white;
    fill.command.width = 1;
    fill.command.height = 1;
    fill.pixels.assign(pixel, pixel + 4);
    fill.rowBytes = 4;
    mWaitingBytes += 4;
    mWaiting.push_back(std::move(fill));
}

uint32_t TextureQueue::create(const TextureDesc& desc, uint32_t sourceSlot, uint32_t sourceMip)
{
    TextureCommand command;
    command.type = TextureCommand::Type::Create;
    command.desc = desc;
    command.sourceSlot = sourceSlot;
    command.sourceMip = sourceMip;

    std::lock_guard<std::mutex> producer(mProducer);
    {
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
    }
    if (mBudget)
    {
        mBudget->reserveAnyway(MemoryCategory::Textures, textureBytes(desc));
    }
    const uint32_t slot = command.slot;
    queue(std::move(command));
    return slot;
}

void TextureQueue::queue(TextureCommand&& command)
{
    if (!mWaiting.empty())
    {
        Waiting behind;
        behind.command = std::move(command);
        mWaiting.push_back(std::move(behind));
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.push_back(std::move(command));
}

bool TextureQueue::reserve(uint64_t bytes, uint64_t& position)
{
    if (!mRing || bytes > mRingSize)
    {
        return false;
    }
    // A piece never wraps: one that would is moved to the start, and the gap goes
    // back with it.
    uint64_t start = mRingHead;
    const uint64_t offset = start % mRingSize;
    if (offset + bytes > mRingSize)
    {
        start += mRingSize - offset;
    }
    const uint64_t end = (start + bytes + UPLOAD_RING_ALIGNMENT - 1) / UPLOAD_RING_ALIGNMENT * UPLOAD_RING_ALIGNMENT;
    if (end - mRingTail.load(std::memory_order_acquire) > mRingSize)
    {
        return false;
    }
    position = start;
    mRingHead = end;
    return true;
}

void TextureQueue::sendPieces(const TextureCommand& upload, uint32_t rowBytes, uint32_t firstRow, uint32_t rows, uint64_t position)
{
    const uint32_t rowsPerPiece = uint32_t(std::max<uint64_t>(mPieceBytes / rowBytes, 1));
    std::lock_guard<std::mutex> lock(mMutex);
    for (uint32_t done = 0; done < rows;)
    {
        const uint32_t count = std::min(rowsPerPiece, rows - done);
        TextureCommand piece = upload;
        piece.y = upload.y + firstRow + done;
        piece.height = count;
        piece.ringOffset = position % mRingSize + uint64_t(done) * rowBytes;
        piece.ringBytes = uint64_t(count) * rowBytes;
        piece.ringEnd = position + uint64_t(done + count) * rowBytes;
        mCommands.push_back(piece);
        done += count;
    }
}

bool TextureQueue::writeRows(Waiting& upload)
{
    const uint32_t height = upload.command.height;
    const uint32_t rowsPerPiece = uint32_t(std::max<uint64_t>(mPieceBytes / upload.rowBytes, 1));
    while (upload.rowsDone < height)
    {
        const uint32_t rows = std::min(rowsPerPiece, height - upload.rowsDone);
        const uint64_t bytes = uint64_t(rows) * upload.rowBytes;
        uint64_t position = 0;
        if (!reserve(bytes, position))
        {
            return false;
        }
        memcpy(mRing + position % mRingSize, upload.pixels.data() + uint64_t(upload.rowsDone) * upload.rowBytes, bytes);
        sendPieces(upload.command, upload.rowBytes, upload.rowsDone, rows, position);
        upload.rowsDone += rows;
        mWaitingBytes -= bytes;
    }
    return true;
}

void TextureQueue::sendWaiting()
{
    while (!mWaiting.empty())
    {
        Waiting& front = mWaiting.front();
        if (front.command.type == TextureCommand::Type::Upload)
        {
            if (!writeRows(front))
            {
                return;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mCommands.push_back(std::move(front.command));
        }
        mWaiting.pop_front();
    }
}

uint8_t* TextureQueue::beginUpload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   uint32_t rowBytes)
{
    mProducer.lock(); // until finishUpload()
    mOpen = Waiting();
    mOpen.command.type = TextureCommand::Type::Upload;
    mOpen.command.slot = slot;
    mOpen.command.level = level;
    mOpen.command.x = x;
    mOpen.command.y = y;
    mOpen.command.width = width;
    mOpen.command.height = height;
    mOpen.rowBytes = rowBytes;
    mOpenInRing = false;
    if (slot == 0 || height == 0 || rowBytes == 0)
    {
        mOpen.command.slot = 0;
        return nullptr;
    }

    // Straight into the ring when nothing waits before it and it fits in one run.
    const uint64_t bytes = uint64_t(rowBytes) * height;
    if (mWaiting.empty() && reserve(bytes, mOpenPosition))
    {
        mOpenInRing = true;
        return mRing + mOpenPosition % mRingSize;
    }
    mOpen.pixels.resize(bytes);
    return mOpen.pixels.data();
}

void TextureQueue::finishUpload()
{
    if (mOpen.command.slot != 0)
    {
        if (mOpenInRing)
        {
            sendPieces(mOpen.command, mOpen.rowBytes, 0, mOpen.command.height, mOpenPosition);
        }
        else
        {
            mWaitingBytes += mOpen.pixels.size();
            mWaiting.push_back(std::move(mOpen));
            sendWaiting();
        }
    }
    mOpen = Waiting();
    mProducer.unlock();
}

uint32_t TextureQueue::freeSlots()
{
    std::lock_guard<std::mutex> lock(mMutex);
    return MAX_TEXTURES - mNextSlot + uint32_t(mFreeSlots.size());
}

void TextureQueue::upload(uint32_t slot, uint32_t level, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          const void* pixels, size_t bytes)
{
    const uint32_t rowBytes = height != 0 ? uint32_t(bytes / height) : 0;
    uint8_t* target = beginUpload(slot, level, x, y, width, height, rowBytes);
    if (target)
    {
        memcpy(target, pixels, size_t(rowBytes) * height);
    }
    finishUpload();
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

    std::lock_guard<std::mutex> producer(mProducer);
    queue(std::move(command));
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

    std::lock_guard<std::mutex> producer(mProducer);
    queue(std::move(command));
}

void TextureQueue::flush()
{
    std::lock_guard<std::mutex> producer(mProducer);
    sendWaiting();
}

void TextureQueue::setUploadRing(uint8_t* memory, uint64_t bytes, uint64_t pieceBytes)
{
    std::lock_guard<std::mutex> producer(mProducer);
    mRing = memory;
    mRingSize = bytes / UPLOAD_RING_ALIGNMENT * UPLOAD_RING_ALIGNMENT;
    mPieceBytes = std::min(pieceBytes, mRingSize);
    sendWaiting();
}

void TextureQueue::take(std::vector<TextureCommand>& commands)
{
    std::lock_guard<std::mutex> lock(mMutex);
    commands.insert(commands.end(), mCommands.begin(), mCommands.end());
    mCommands.clear();
}

void TextureQueue::releaseRing(uint64_t ringEnd)
{
    uint64_t tail = mRingTail.load(std::memory_order_relaxed);
    while (ringEnd > tail && !mRingTail.compare_exchange_weak(tail, ringEnd, std::memory_order_release, std::memory_order_relaxed))
    {
    }
}

void TextureQueue::releaseSlot(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeSlots.push_back(slot);
}

} // namespace asr
