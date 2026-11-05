/**
 * asrscenefile.h
 * Scene capture files: the viewer dumps a scene, and asrenderer_test replays it
 * at a fixed camera for repeatable speed, memory and side-by-side checks.
 *
 * A file is a header, then a chunk table, then the chunk data. Chunks are found
 * through the table, so a reader skips any chunk it does not know, and a new
 * chunk type never breaks old files. All values are little-endian. Each chunk
 * type has its own version, for changing its layout later.
 *
 * Only the camera chunk has a fixed layout so far. The other chunk layouts are
 * defined with the GPU scene tables in P3.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace asr
{

constexpr uint32_t fourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t SCENE_FILE_MAGIC = fourCC('A', 'S', 'R', 'S');
constexpr uint32_t SCENE_FILE_VERSION = 1;

enum SceneChunkType : uint32_t
{
    SCENE_CHUNK_CAMERA = fourCC('C', 'A', 'M', 'R'),
    SCENE_CHUNK_GEOMETRY = fourCC('G', 'E', 'O', 'M'),
    SCENE_CHUNK_MATERIALS = fourCC('M', 'A', 'T', 'L'),
    SCENE_CHUNK_TEXTURES = fourCC('T', 'E', 'X', 'R'),
    SCENE_CHUNK_INSTANCES = fourCC('I', 'N', 'S', 'T'),
    SCENE_CHUNK_LIGHTS = fourCC('L', 'G', 'H', 'T'),
    SCENE_CHUNK_ENVIRONMENT = fourCC('E', 'N', 'V', 'I'),
};

// Fixed layout of a camera chunk, version 1.
struct SceneCamera
{
    float position[3];
    float forward[3];
    float up[3];
    float verticalFovRadians;
    float nearClip;
    float farClip;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(SceneCamera) == 56, "SceneCamera is a file layout: never let it change silently");

struct SceneChunk
{
    uint32_t type = 0;
    uint32_t version = 1;
    std::vector<uint8_t> data;
};

struct SceneFile
{
    std::vector<SceneChunk> chunks;

    // The first chunk of a type, or null.
    const SceneChunk* find(uint32_t type) const;

    // Both return false and set error on failure. load() checks every size and
    // offset, so a damaged file gives an error instead of a crash.
    bool save(const std::string& path, std::string& error) const;
    bool load(const std::string& path, std::string& error);
};

} // namespace asr
