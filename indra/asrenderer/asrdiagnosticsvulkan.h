/**
 * asrdiagnosticsvulkan.h
 * Device-loss evidence using the existing driver's optional NV checkpoints and the last
 * completed scene readback. No extra synchronization or GPU readback is introduced.
 * Author: chanayane@firestorm
 */
#pragma once

#include "asrscene.h"
#include "volk.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace asr
{

// An opaque marker encodes its frame, stable pass ID and before/after bit, rather than
// pointing into frame-slot storage that could be overwritten while a device is failing.
// Pass names must stay alive: the frame graph uses string literals. Recording only writes
// one fixed array entry; it allocates nothing and performs no name lookup.
template<uint32_t Passes>
class VulkanDiagnostics
{
    static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "checkpoint frame tokens require a 64-bit process");
    static_assert(Passes <= 128, "checkpoint tokens reserve seven bits for the pass ID");

public:
    void initialize(PFN_vkCmdSetCheckpointNV set, PFN_vkGetQueueCheckpointDataNV get)
    {
        mSet = set;
        mGet = get;
    }

    bool checkpointsEnabled() const { return mSet && mGet; }

    void beginFrame(uint64_t frame)
    {
        mFrame = frame;
    }

    void checkpoint(VkCommandBuffer commands, uint32_t id, const char* pass, bool after = false)
    {
        if (!checkpointsEnabled() || id >= Passes)
        {
            return;
        }
        mPassNames[id] = pass;
        // Adding one keeps the marker non-null, including frame zero/pass zero/before.
        const uintptr_t token = ((uintptr_t(mFrame) << 8) | (uintptr_t(id) << 1) | uintptr_t(after)) + 1;
        mSet(commands, reinterpret_cast<const void*>(token));
    }

    // Reuses the existing fence-completed readback, before its mapped view is rewritten.
    void completedScene(uint64_t frame, const GpuSceneCounts& counts, const GpuSceneView& view)
    {
        mCompletedFrame = frame;
        mCounts = counts;
        mView = view;
        mHaveCompleted = true;
    }

    // Vulkan permits the checkpoint query only after VK_ERROR_DEVICE_LOST. Pipeline stage
    // markers narrow a range of commands; they do not identify a faulting instruction.
    std::string describeLostDevice(VkQueue queue) const
    {
        std::string text;
        if (checkpointsEnabled())
        {
            uint32_t count = 0;
            mGet(queue, &count, nullptr);
            std::vector<VkCheckpointDataNV> data(count);
            for (VkCheckpointDataNV& entry : data)
            {
                entry.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            }
            if (count != 0)
            {
                mGet(queue, &count, data.data());
            }
            text += "; GPU checkpoints " + std::to_string(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uintptr_t token = reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker);
                const uintptr_t decoded = token - 1;
                const uint32_t id = uint32_t((decoded >> 1) & 127);
                const char* stage = data[i].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT ? "top" :
                                    data[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT ? "bottom" : "other";
                text += std::string("; ") + stage + " stage " + std::to_string(uint32_t(data[i].stage));
                if (token != 0 && id < Passes && mPassNames[id])
                {
                    text += " frame " + std::to_string(decoded >> 8) + ((decoded & 1) ? " after " : " before ") + mPassNames[id];
                }
                else
                {
                    text += " unknown marker";
                }
            }
        }
        else
        {
            text += "; GPU checkpoints unavailable";
        }
        if (!mHaveCompleted)
        {
            return text + "; no completed scene readback";
        }

        text += "; last completed scene frame " + std::to_string(mCompletedFrame) +
                " view " + std::to_string(uint32_t(mView.pixelSize[0])) + "x" + std::to_string(uint32_t(mView.pixelSize[1]));
        uint64_t vertices = 0;
        uint64_t clusters = 0;
        for (uint32_t list = sceneList(SCENE_BLENDED_KIND, 0); list < SCENE_LISTS; ++list)
        {
            const GpuDrawArguments& early = mCounts.early[list];
            const GpuDrawArguments& late = mCounts.late[list];
            const uint64_t instances = uint64_t(early.instanceCount) + late.instanceCount;
            const uint32_t end = list + 1 < SCENE_LISTS ? mView.listBase[list + 1] : mView.visibleEnd;
            const uint64_t room = end - mView.listBase[list];
            const uint32_t expected = CLUSTER_BUCKET_TRIANGLES[list % CLUSTER_BUCKETS] * 3;
            const uint32_t tailList = list - sceneList(SCENE_BLENDED_KIND, 0);
            const GpuDrawArguments& tailEarly = mCounts.macoitTail[tailList];
            const GpuDrawArguments& tailLate = mCounts.macoitTail[SCENE_BLENDED_LISTS + tailList];
            const auto validTail = [this](const GpuDrawArguments& gated, const GpuDrawArguments& original)
            {
                return gated.vertexCount == original.vertexCount && gated.firstVertex == original.firstVertex &&
                       gated.firstInstance == original.firstInstance &&
                       gated.instanceCount == (mCounts.macoitTailSeen ? original.instanceCount : 0);
            };
            vertices += uint64_t(early.vertexCount) * early.instanceCount + uint64_t(late.vertexCount) * late.instanceCount;
            clusters += instances;
            const bool invalid = end < mView.listBase[list] || instances > room || early.vertexCount != expected || late.vertexCount != expected ||
                                 early.firstVertex != 0 || late.firstVertex != 0 || early.firstInstance != 0 ||
                                 (late.instanceCount != 0 && late.firstInstance != early.instanceCount) ||
                                 !validTail(tailEarly, early) || !validTail(tailLate, late);
            if (instances != 0 || invalid)
            {
                text += "; blend list " + std::to_string(list) + " early/late " + std::to_string(early.instanceCount) +
                        "/" + std::to_string(late.instanceCount) + " room " + std::to_string(room);
                if (invalid)
                {
                    text += " INVALID DRAW vertices " + std::to_string(early.vertexCount) + "/" + std::to_string(late.vertexCount) +
                            " tail counts " + std::to_string(tailEarly.instanceCount) + "/" + std::to_string(tailLate.instanceCount);
                }
            }
        }
        text += "; blended clusters " + std::to_string(clusters) + " submitted vertices/pass " + std::to_string(vertices) +
                "; tail " + std::to_string(mCounts.macoitTailSeen) + "; dropped " + std::to_string(mCounts.dropped);
        return text;
    }

private:
    PFN_vkCmdSetCheckpointNV mSet = nullptr;
    PFN_vkGetQueueCheckpointDataNV mGet = nullptr;
    std::array<const char*, Passes> mPassNames{};
    uint64_t mFrame = 0;
    uint64_t mCompletedFrame = 0;
    GpuSceneCounts mCounts{};
    GpuSceneView mView{};
    bool mHaveCompleted = false;
};

} // namespace asr
