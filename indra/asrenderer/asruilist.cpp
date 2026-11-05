/**
 * asruilist.cpp
 * See asruilist.h.
 */
#include "asruilist.h"

#include <algorithm>

namespace asr
{

namespace
{

UIPrimitive primitiveOf(UIGeometry geometry)
{
    switch (geometry)
    {
        case UIGeometry::Points:
            return UIPrimitive::Points;
        case UIGeometry::Lines:
        case UIGeometry::LineStrip:
        case UIGeometry::LineLoop:
            return UIPrimitive::Lines;
        default:
            return UIPrimitive::Triangles;
    }
}

} // namespace

bool resolveScissor(const UIScissor& scissor, uint32_t listWidth, uint32_t listHeight,
                    uint32_t targetWidth, uint32_t targetHeight, UIRect& rect)
{
    // What both the list and the target cover.
    const uint32_t visibleWidth = std::min(listWidth, targetWidth);
    const uint32_t visibleHeight = std::min(listHeight, targetHeight);
    if (!scissor.enabled)
    {
        rect = { 0, 0, visibleWidth, visibleHeight };
        return visibleWidth != 0 && visibleHeight != 0;
    }

    // In 64 bits, because GL lets a rectangle reach far past the window.
    const int64_t width = visibleWidth;
    const int64_t height = visibleHeight;
    const int64_t left = std::clamp<int64_t>(scissor.x, 0, width);
    const int64_t right = std::clamp<int64_t>(int64_t(scissor.x) + scissor.width, 0, width);
    const int64_t top = std::clamp<int64_t>(int64_t(listHeight) - (int64_t(scissor.y) + scissor.height), 0, height);
    const int64_t bottom = std::clamp<int64_t>(int64_t(listHeight) - scissor.y, 0, height);
    rect = { uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top) };
    return rect.width != 0 && rect.height != 0;
}

void UIList::clear()
{
    mVertices.clear();
    mDraws.clear();
    mBlends.clear();
    mScissor = UIScissor();
    mBlend = 0;
}

void UIList::setBlend(const UIBlend& blend)
{
    const uint32_t key = blend.key();
    if (!mBlends.empty() && mBlends[mBlend].key() == key)
    {
        return;
    }
    // UI code uses a handful of blends, and this runs only when one changes.
    for (size_t i = 0; i < mBlends.size(); ++i)
    {
        if (mBlends[i].key() == key)
        {
            mBlend = uint16_t(i);
            return;
        }
    }
    mBlend = uint16_t(mBlends.size());
    mBlends.push_back(blend);
}

void UIList::setScissor(const UIScissor& scissor)
{
    mScissor = scissor;
}

UIDraw& UIList::drawFor(UIPrimitive primitive)
{
    if (mBlends.empty())
    {
        mBlends.push_back(UIBlend());
    }
    if (!mDraws.empty())
    {
        UIDraw& last = mDraws.back();
        if (last.primitive == primitive && last.blend == mBlend && last.scissor == mScissor)
        {
            return last;
        }
    }
    UIDraw draw;
    draw.firstVertex = uint32_t(mVertices.size());
    draw.scissor = mScissor;
    draw.blend = mBlend;
    draw.primitive = primitive;
    mDraws.push_back(draw);
    return mDraws.back();
}

void UIList::append(UIGeometry geometry, const UIVertex* in, uint32_t count)
{
    // Keeps only whole primitives, as a GL draw does.
    uint32_t added = 0;
    switch (geometry)
    {
        case UIGeometry::Triangles:     added = count / 3 * 3; break;
        case UIGeometry::TriangleStrip:
        case UIGeometry::TriangleFan:   added = count >= 3 ? (count - 2) * 3 : 0; break;
        case UIGeometry::Points:        added = count; break;
        case UIGeometry::Lines:         added = count / 2 * 2; break;
        case UIGeometry::LineStrip:     added = count >= 2 ? (count - 1) * 2 : 0; break;
        case UIGeometry::LineLoop:      added = count >= 2 ? count * 2 : 0; break;
    }
    if (added == 0)
    {
        return;
    }

    UIDraw& draw = drawFor(primitiveOf(geometry));
    const size_t start = mVertices.size();
    mVertices.resize(start + added);
    UIVertex* out = mVertices.data() + start;

    switch (geometry)
    {
        case UIGeometry::Triangles:
        case UIGeometry::Points:
        case UIGeometry::Lines:
            std::copy(in, in + added, out);
            break;
        case UIGeometry::TriangleStrip:
            for (uint32_t i = 2; i < count; ++i)
            {
                *out++ = in[i - 2];
                *out++ = in[i - 1];
                *out++ = in[i];
            }
            break;
        case UIGeometry::TriangleFan:
            for (uint32_t i = 2; i < count; ++i)
            {
                *out++ = in[0];
                *out++ = in[i - 1];
                *out++ = in[i];
            }
            break;
        case UIGeometry::LineStrip:
        case UIGeometry::LineLoop:
            for (uint32_t i = 1; i < count; ++i)
            {
                *out++ = in[i - 1];
                *out++ = in[i];
            }
            if (geometry == UIGeometry::LineLoop)
            {
                *out++ = in[count - 1];
                *out++ = in[0];
            }
            break;
    }
    draw.vertexCount += added;
}

void UIList::append(UIGeometry geometry, const UIStreams& streams, uint32_t count)
{
    if (count == 0)
    {
        return;
    }
    mTransformed.resize(count);
    const float* const m = streams.matrix;
    const float* const t = streams.uvMatrix;
    const char* position = reinterpret_cast<const char*>(streams.positions);

    for (uint32_t i = 0; i < count; ++i, position += streams.positionStride)
    {
        const float* const p = reinterpret_cast<const float*>(position);
        UIVertex& v = mTransformed[i];
        v.position[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        v.position[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        v.position[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
        v.position[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];

        const float u = streams.uvs[i * 2];
        const float w = streams.uvs[i * 2 + 1];
        v.uv[0] = t ? t[0] * u + t[4] * w + t[12] : u;
        v.uv[1] = t ? t[1] * u + t[5] * w + t[13] : w;

        v.color = streams.colors ? streams.colors[i] : streams.singleColor;
        v.texture = streams.texture;
    }
    append(geometry, mTransformed.data(), count);
}

} // namespace asr
