/**
 * asrscenefile.cpp
 * Reads and writes scene capture files. Layout is described in asrscenefile.h.
 */
#include "asrscenefile.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace asr
{

namespace
{

struct FileHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t chunkCount;
    uint32_t reserved;
};

struct TableEntry
{
    uint32_t type;
    uint32_t version;
    uint64_t offset;
    uint64_t size;
};

static_assert(sizeof(FileHeader) == 16 && sizeof(TableEntry) == 24, "file layout");

// Closes the file at the end of the scope, on every return path.
struct FileCloser
{
    void operator()(FILE* file) const { fclose(file); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr openFile(const std::string& path, const char* mode)
{
    return FilePtr(fopen(path.c_str(), mode));
}

} // namespace

const SceneChunk* SceneFile::find(uint32_t type) const
{
    for (const SceneChunk& chunk : chunks)
    {
        if (chunk.type == type)
        {
            return &chunk;
        }
    }
    return nullptr;
}

bool SceneFile::save(const std::string& path, std::string& error) const
{
    FilePtr file = openFile(path, "wb");
    if (!file)
    {
        error = "cannot create " + path;
        return false;
    }

    FileHeader header{ SCENE_FILE_MAGIC, SCENE_FILE_VERSION, uint32_t(chunks.size()), 0 };
    std::vector<TableEntry> table;
    table.reserve(chunks.size());
    uint64_t offset = sizeof(FileHeader) + chunks.size() * sizeof(TableEntry);
    for (const SceneChunk& chunk : chunks)
    {
        table.push_back({ chunk.type, chunk.version, offset, chunk.data.size() });
        offset += chunk.data.size();
    }

    bool ok = fwrite(&header, sizeof(header), 1, file.get()) == 1;
    if (ok && !table.empty())
    {
        ok = fwrite(table.data(), sizeof(TableEntry), table.size(), file.get()) == table.size();
    }
    for (const SceneChunk& chunk : chunks)
    {
        if (ok && !chunk.data.empty())
        {
            ok = fwrite(chunk.data.data(), 1, chunk.data.size(), file.get()) == chunk.data.size();
        }
    }
    if (!ok)
    {
        error = "cannot write " + path;
    }
    return ok;
}

bool SceneFile::load(const std::string& path, std::string& error)
{
    chunks.clear();
    FilePtr file = openFile(path, "rb");
    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }

    // 64-bit seek, because a captured scene can pass 2 GB.
#ifdef _WIN32
    _fseeki64(file.get(), 0, SEEK_END);
    const uint64_t fileSize = uint64_t(_ftelli64(file.get()));
    _fseeki64(file.get(), 0, SEEK_SET);
#else
    fseeko(file.get(), 0, SEEK_END);
    const uint64_t fileSize = uint64_t(ftello(file.get()));
    fseeko(file.get(), 0, SEEK_SET);
#endif

    FileHeader header;
    if (fread(&header, sizeof(header), 1, file.get()) != 1 || header.magic != SCENE_FILE_MAGIC)
    {
        error = path + " is not a scene file";
        return false;
    }
    if (header.version != SCENE_FILE_VERSION)
    {
        error = path + " has unsupported version " + std::to_string(header.version);
        return false;
    }

    const uint64_t tableBytes = uint64_t(header.chunkCount) * sizeof(TableEntry);
    if (tableBytes > fileSize - sizeof(FileHeader))
    {
        error = path + " has a damaged chunk table";
        return false;
    }

    std::vector<TableEntry> table(header.chunkCount);
    if (!table.empty() && fread(table.data(), sizeof(TableEntry), table.size(), file.get()) != table.size())
    {
        error = path + " has a damaged chunk table";
        return false;
    }

    const uint64_t dataStart = sizeof(FileHeader) + tableBytes;
    chunks.resize(table.size());
    for (size_t i = 0; i < table.size(); ++i)
    {
        const TableEntry& entry = table[i];
        // Written this way so a huge size cannot overflow the sum.
        if (entry.offset < dataStart || entry.offset > fileSize || entry.size > fileSize - entry.offset)
        {
            chunks.clear();
            error = path + " has a damaged chunk " + std::to_string(i);
            return false;
        }

        SceneChunk& chunk = chunks[i];
        chunk.type = entry.type;
        chunk.version = entry.version;
        chunk.data.resize(size_t(entry.size));
#ifdef _WIN32
        _fseeki64(file.get(), int64_t(entry.offset), SEEK_SET);
#else
        fseeko(file.get(), off_t(entry.offset), SEEK_SET);
#endif
        if (!chunk.data.empty() && fread(chunk.data.data(), 1, chunk.data.size(), file.get()) != chunk.data.size())
        {
            chunks.clear();
            error = path + " is truncated";
            return false;
        }
    }
    return true;
}

} // namespace asr
