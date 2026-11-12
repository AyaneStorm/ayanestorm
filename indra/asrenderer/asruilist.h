/**
 * asruilist.h
 * One finished frame of UI: every vertex in one array, and as few draws as the
 * state changes allow. The main thread fills it and the render thread replays it,
 * so nothing here touches a GPU API.
 *
 * Draws merge because everything that would split them in GL lives in the vertex:
 * positions are transformed before they are stored, strips and fans become plain
 * triangles, and each vertex names its own texture. Only a change of primitive,
 * blend or scissor starts a new draw.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace asr
{

// Also the size of the texture array in asrUIF.glsl.
constexpr uint32_t MAX_TEXTURES = 65536;

// How far down the UI viewport sits, in pixels. At a fractional UI scale a horizontal
// edge can land exactly on a pixel centre, and GL then keeps that row at the bottom
// of a shape where GPU APIs keep it at the top. This small step decides every such
// tie the way GL does. It is above the 1/256 pixel GPUs snap to, and too small to see.
constexpr float UI_EDGE_NUDGE = 1.f / 128.f;

// How UIVertex::texture packs its fields.
constexpr uint32_t UI_TEXTURE_SLOT_MASK = 0xFFFFF;
constexpr uint32_t UI_SAMPLER_SHIFT = 28;
// The position is in world space (w 1), not clip space: the device projects it with the
// camera it draws the scene with this frame, so it holds still while that camera eases
// on from the one the list was made with (zoom, prediction), and between list updates.
constexpr uint32_t UI_WORLD_BIT = 1u << 27;
constexpr uint32_t UI_SOLID_COLOR_BIT = 1u << 31; // keep the vertex color, take alpha from the texture

enum class UISampler : uint32_t
{
    LinearClamp,
    LinearWrap,
    NearestClamp,
    NearestWrap,
    Count
};

inline uint32_t uiTextureBits(uint32_t slot, UISampler sampler = UISampler::LinearClamp, bool solidColor = false)
{
    return slot | (uint32_t(sampler) << UI_SAMPLER_SHIFT) | (solidColor ? UI_SOLID_COLOR_BIT : 0);
}

struct UIVertex
{
    float position[4]; // already in clip space, with GL's axes
    float uv[2];
    uint32_t color;    // RGBA8
    uint32_t texture;  // see uiTextureBits()
};
static_assert(sizeof(UIVertex) == 32, "the pipelines describe this layout");

// In the order of LLRender::eGeomModes.
enum class UIGeometry : uint8_t
{
    Triangles,
    TriangleStrip,
    TriangleFan,
    Points,
    Lines,
    LineStrip,
    LineLoop
};

// What the GPU draws: UIGeometry after strips, fans and loops are expanded.
enum class UIPrimitive : uint8_t
{
    Triangles,
    Lines,
    Points,
    Count
};

// In the order of LLRender::eBlendFactor.
enum class UIBlendFactor : uint8_t
{
    One,
    Zero,
    DestColor,
    SourceColor,
    OneMinusDestColor,
    OneMinusSourceColor,
    DestAlpha,
    SourceAlpha,
    OneMinusDestAlpha,
    OneMinusSourceAlpha
};

struct UIBlend
{
    UIBlendFactor colorSource = UIBlendFactor::SourceAlpha;
    UIBlendFactor colorDest = UIBlendFactor::OneMinusSourceAlpha;
    UIBlendFactor alphaSource = UIBlendFactor::One;
    UIBlendFactor alphaDest = UIBlendFactor::OneMinusSourceAlpha;

    uint32_t key() const
    {
        return uint32_t(colorSource) | uint32_t(colorDest) << 4 | uint32_t(alphaSource) << 8 | uint32_t(alphaDest) << 12;
    }
};

// In pixels, with GL's origin at the bottom left.
struct UIScissor
{
    bool enabled = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    bool operator==(const UIScissor& other) const
    {
        return enabled == other.enabled && x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

// A scissor as GPU APIs want it: inside the target, with the origin at the top left.
struct UIRect
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// The scissor belongs to the list's size, and the target can differ from it while
// the window is being resized. Returns false when nothing of the target remains, and
// the draw is skipped.
bool resolveScissor(const UIScissor& scissor, uint32_t listWidth, uint32_t listHeight,
                    uint32_t targetWidth, uint32_t targetHeight, UIRect& rect);

// In GL's order (GL_NEVER + n). The list stores them flipped for reversed depth.
enum class UICompare : uint8_t
{
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always
};

enum class UICull : uint8_t
{
    None,
    Front,
    Back
};

// A draw's depth state. GL writes depth only while it tests, and so does this.
struct UIDepth
{
    bool test = false;
    bool write = false;
    UICompare compare = UICompare::GreaterOrEqual; // reversed: GL's GL_LEQUAL

    bool operator==(const UIDepth& other) const
    {
        return test == other.test && write == other.write && compare == other.compare;
    }
};

class UIList;

// For asrUIV.glsl: world space to the list's clip space, through viewProjection (the
// camera drawn this frame, column-major, Y down, reversed depth) into the scene's place
// in the window (pixels, from the top left). With no camera, all zeros: nothing shows.
void uiWorldToWindow(const float* viewProjection, const UIList& ui, const float sceneSize[2], const float sceneOffset[2],
                     float out[16]);

struct UIDraw
{
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    UIScissor scissor;
    uint16_t blend = 0; // index into UIList::blends()
    UIPrimitive primitive = UIPrimitive::Triangles;
    UIDepth depth;
    UICull cull = UICull::None;
    bool clearDepth = false; // no vertices: clears the depth to farthest, as glClear
};

// The vertices LLRender holds at a flush: separate arrays, not yet transformed.
struct UIStreams
{
    const float* positions = nullptr;    // x, y, z per vertex
    uint32_t positionStride = 16;        // bytes
    const float* uvs = nullptr;          // two floats per vertex
    const uint32_t* colors = nullptr;    // RGBA8 per vertex; null uses singleColor
    uint32_t singleColor = 0xFFFFFFFF;
    const float* matrix = nullptr;       // column-major 4x4, to clip space, or world space when world is set
    bool world = false;                  // see UI_WORLD_BIT
    const float* uvMatrix = nullptr;     // column-major 4x4; null means none
    uint32_t texture = 0;                // see uiTextureBits()
};

class UIList
{
public:
    // Keeps the memory, so a list reused every frame stops allocating.
    void clear();

    // The window size in pixels this list is recorded for. It is drawn at that size
    // from the top left corner, whatever the window has become since, so a list that
    // outlives a resize stays sharp and correct until the next one arrives.
    void setSize(uint32_t width, uint32_t height)
    {
        mWidth = width;
        mHeight = height;
    }
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }

    void setBlend(const UIBlend& blend);
    void setScissor(const UIScissor& scissor);

    // GL depth and cull state, as glEnable, glDepthFunc, glDepthMask and glCullFace set it.
    void setDepthTest(bool enabled) { mDepthTest = enabled; }
    void setDepthWrite(bool enabled) { mDepthWrite = enabled; }
    void setDepthCompare(UICompare glCompare);
    void setCullEnabled(bool enabled) { mCullEnabled = enabled; }
    void setCullFace(UICull face) { mCullFace = face; }
    void clearDepth();

    // glViewport, in GL window pixels (Y up). Folded into the vertices' clip x and y,
    // as the list draws in one viewport of its full size.
    void setViewport(int32_t x, int32_t y, int32_t width, int32_t height);

    // World-space vertices (UI_WORLD_BIT) get the scene's own depth from the device's
    // projection. Other depth-tested vertices keep GL's window depth, reversed, so they
    // compare with each other and with the HUD's depth as in OpenGL. beginWorld() opens
    // the part drawn before the HUD, against the scene depth; markHud() is where the HUD
    // goes: the draws before it are drawn before the HUD, the rest after.
    void beginWorld();
    void markHud();
    bool hasWorld() const { return mHasWorld; } // some vertices are in world space
    uint32_t hudDraw() const { return mHudDraw; }      // the first draw after the HUD
    bool worldDepthTested() const { return mWorldDepthTested; } // a draw before the HUD tests depth

    // Adds vertices that are already final.
    void append(UIGeometry geometry, const UIVertex* vertices, uint32_t count);

    // Transforms and adds.
    void append(UIGeometry geometry, const UIStreams& streams, uint32_t count);

    const std::vector<UIVertex>& vertices() const { return mVertices; }
    const std::vector<UIDraw>& draws() const { return mDraws; }
    const std::vector<UIBlend>& blends() const { return mBlends; }

    // Tells lists apart: the render thread copies a list only once per generation.
    uint64_t generation = 0;

private:
    // The draw new vertices go into, started anew when the state differs.
    UIDraw& drawFor(UIPrimitive primitive);

    std::vector<UIVertex> mVertices;
    std::vector<UIVertex> mTransformed;
    std::vector<UIDraw> mDraws;
    std::vector<UIBlend> mBlends;
    UIScissor mScissor;
    uint16_t mBlend = 0;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    bool mDepthTest = false;
    bool mDepthWrite = true;
    UICompare mDepthCompare = UICompare::GreaterOrEqual;
    bool mCullEnabled = false;
    UICull mCullFace = UICull::Back;
    int32_t mViewport[4] = { 0, 0, 0, 0 }; // zero width: the list's full size
    bool mInWorld = false;                 // between beginWorld() and markHud()
    uint32_t mHudDraw = 0;
    bool mWorldDepthTested = false;
    bool mHasWorld = false;
};

} // namespace asr
