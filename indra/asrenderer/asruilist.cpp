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

void uiWorldToWindow(const float* viewProjection, const UIList& ui, const float sceneSize[2], const float sceneOffset[2],
                     float out[16])
{
    std::fill(out, out + 16, 0.f);
    if (!viewProjection || ui.width() == 0 || ui.height() == 0)
    {
        return;
    }
    // The scene's clip space to the list's: x' = x * s + w * o, as in setViewport().
    const float scale[2] = { sceneSize[0] / float(ui.width()), sceneSize[1] / float(ui.height()) };
    const float offset[2] = { (2.f * sceneOffset[0] + sceneSize[0]) / float(ui.width()) - 1.f,
                              (2.f * sceneOffset[1] + sceneSize[1]) / float(ui.height()) - 1.f };
    for (uint32_t column = 0; column < 4; ++column)
    {
        const float* in = viewProjection + column * 4;
        float* o = out + column * 4;
        o[0] = in[0] * scale[0] + in[3] * offset[0];
        o[1] = in[1] * scale[1] + in[3] * offset[1];
        o[2] = in[2];
        o[3] = in[3];
    }
}

void UIList::clear()
{
    mVertices.clear();
    mDraws.clear();
    mBlends.clear();
    mScissor = UIScissor();
    mBlend = 0;
    mDepthTest = false;
    mDepthWrite = true;
    mDepthCompare = UICompare::GreaterOrEqual;
    mCullEnabled = false;
    mCullFace = UICull::Back;
    mViewport[2] = 0;
    mInWorld = false;
    mHudDraw = 0;
    mWorldDepthTested = false;
    mHasWorld = false;
}

void UIList::setDepthCompare(UICompare glCompare)
{
    // Reversed depth: nearer is larger, so each compare turns around.
    static constexpr UICompare REVERSED[8] = { UICompare::Never, UICompare::Greater, UICompare::Equal,
                                               UICompare::GreaterOrEqual, UICompare::Less, UICompare::NotEqual,
                                               UICompare::LessOrEqual, UICompare::Always };
    mDepthCompare = REVERSED[uint32_t(glCompare) & 7];
}

void UIList::setViewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    const bool full = x == 0 && y == 0 && uint32_t(width) == mWidth && uint32_t(height) == mHeight;
    mViewport[0] = x;
    mViewport[1] = y;
    mViewport[2] = full || width <= 0 || height <= 0 ? 0 : width;
    mViewport[3] = height;
}

void UIList::clearDepth()
{
    UIDraw draw;
    draw.firstVertex = uint32_t(mVertices.size());
    draw.clearDepth = true;
    mDraws.push_back(draw);
}

void UIList::beginWorld()
{
    mInWorld = true;
}

void UIList::markHud()
{
    mInWorld = false;
    mHudDraw = uint32_t(mDraws.size());
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
    UIDepth depth;
    depth.test = mDepthTest;
    depth.write = mDepthTest && mDepthWrite;
    depth.compare = mDepthCompare;
    // Culling only means something to triangles, and GL culls none of the rest.
    const UICull cull = mCullEnabled && primitive == UIPrimitive::Triangles ? mCullFace : UICull::None;
    // GL clips to the viewport too: with one smaller than the list, the scissor keeps to it.
    UIScissor scissor = mScissor;
    if (mViewport[2] != 0)
    {
        const int32_t left = scissor.enabled ? std::max(scissor.x, mViewport[0]) : mViewport[0];
        const int32_t bottom = scissor.enabled ? std::max(scissor.y, mViewport[1]) : mViewport[1];
        const int32_t right = scissor.enabled ? std::min(scissor.x + scissor.width, mViewport[0] + mViewport[2]) : mViewport[0] + mViewport[2];
        const int32_t top = scissor.enabled ? std::min(scissor.y + scissor.height, mViewport[1] + mViewport[3]) : mViewport[1] + mViewport[3];
        scissor = { true, left, bottom, std::max(right - left, 0), std::max(top - bottom, 0) };
    }
    // A draw belongs to the part before the HUD or after it, never to both.
    if (!mDraws.empty() && mDraws.size() != mHudDraw)
    {
        UIDraw& last = mDraws.back();
        if (!last.clearDepth && last.primitive == primitive && last.blend == mBlend && last.scissor == scissor &&
            last.depth == depth && last.cull == cull)
        {
            return last;
        }
    }
    UIDraw draw;
    draw.firstVertex = uint32_t(mVertices.size());
    draw.scissor = scissor;
    draw.blend = mBlend;
    draw.primitive = primitive;
    draw.depth = depth;
    draw.cull = cull;
    mDraws.push_back(draw);
    if (mInWorld && depth.test)
    {
        mWorldDepthTested = true;
    }
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
                // GL turns every other strip triangle round so all keep one winding: culling needs it.
                const bool odd = (i & 1) != 0;
                *out++ = in[odd ? i - 1 : i - 2];
                *out++ = in[odd ? i - 2 : i - 1];
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

    UIVertex* const first = mVertices.data() + start;
    if ((in[0].texture & UI_WORLD_BIT) != 0)
    {
        // The device projects these into the world view: nothing to fold or rewrite.
        mHasWorld = true;
        draw.vertexCount += added;
        return;
    }
    if (mViewport[2] != 0 && mWidth != 0 && mHeight != 0)
    {
        // From the viewport's clip space to the list's: x' = x * sx + w * ox.
        const float sx = float(mViewport[2]) / float(mWidth);
        const float sy = float(mViewport[3]) / float(mHeight);
        const float ox = float(2 * mViewport[0] + mViewport[2]) / float(mWidth) - 1.f;
        const float oy = float(2 * mViewport[1] + mViewport[3]) / float(mHeight) - 1.f;
        for (UIVertex* v = first; v != first + added; ++v)
        {
            v->position[0] = v->position[0] * sx + v->position[3] * ox;
            v->position[1] = v->position[1] * sy + v->position[3] * oy;
        }
    }
    if (draw.depth.test)
    {
        // The UI vertex shader makes depth (z + w) / 2; the z written here makes it
        // one less GL's window depth, reversed as the renderer's.
        for (UIVertex* v = first; v != first + added; ++v)
        {
            const float w = v->position[3];
            const float wanted = 0.5f * (w - v->position[2]);
            v->position[2] = 2.f * wanted - w;
        }
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
        v.texture = streams.world ? streams.texture | UI_WORLD_BIT : streams.texture;
    }
    append(geometry, mTransformed.data(), count);
}

} // namespace asr
