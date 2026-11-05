/**
 * @file asrgltable.cpp
 * @brief See asrgltable.h.
 */
#include "linden_common.h"

#include "asrgltable.h"

#include "llglheaders.h"

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace
{

bool sActive = false;
std::string sVendor;
std::string sRenderer;

// Names start high, so a stray 0 or a low constant cannot pass for one.
std::atomic<GLuint> sNextName{ 1000 };

// --- Functions that reply like a driver ---------------------------------------

const GLubyte* APIENTRY asrGetString(GLenum name)
{
    switch (name)
    {
        case GL_VENDOR:                   return (const GLubyte*)sVendor.c_str();
        case GL_RENDERER:                 return (const GLubyte*)sRenderer.c_str();
        case GL_VERSION:                  return (const GLubyte*)"4.6.0 AyaneStorm";
        case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"4.60 AyaneStorm";
        default:                          return (const GLubyte*)"";
    }
}

const GLubyte* APIENTRY asrGetStringi(GLenum, GLuint)
{
    return (const GLubyte*)"";
}

void APIENTRY asrGetIntegerv(GLenum name, GLint* value)
{
    switch (name)
    {
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
            value[0] = value[1] = value[2] = value[3] = 0;
            return;
        case GL_MAX_TEXTURE_SIZE:
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE:          *value = 16384; return;
        case GL_MAX_3D_TEXTURE_SIZE:
        case GL_MAX_ARRAY_TEXTURE_LAYERS:           *value = 2048; return;
        case GL_MAX_ELEMENTS_VERTICES:
        case GL_MAX_ELEMENTS_INDICES:               *value = 1 << 24; return;
        case GL_MAX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_VARYING_VECTORS:                *value = 32; return;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:   *value = 192; return;
        case GL_MAX_VERTEX_ATTRIBS:                 *value = 16; return;
        case GL_MAX_SAMPLES:
        case GL_MAX_COLOR_TEXTURE_SAMPLES:
        case GL_MAX_DEPTH_TEXTURE_SAMPLES:
        case GL_MAX_INTEGER_SAMPLES:
        case GL_MAX_COLOR_ATTACHMENTS:
        case GL_MAX_DRAW_BUFFERS:                   *value = 8; return;
        case GL_MAX_SAMPLE_MASK_WORDS:              *value = 1; return;
        case GL_MAX_UNIFORM_BLOCK_SIZE:             *value = 65536; return;
        default:                                    *value = 0; return;
    }
}

void APIENTRY asrGetBooleanv(GLenum, GLboolean* value)
{
    *value = GL_FALSE;
}

void APIENTRY asrGetFloatv(GLenum name, GLfloat* value)
{
    switch (name)
    {
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
            value[0] = value[1] = 1.f;
            return;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY:
            *value = 16.f;
            return;
        default:
            *value = 0.f;
            return;
    }
}

void APIENTRY asrGetDoublev(GLenum, GLdouble* value)
{
    *value = 0.0;
}

void APIENTRY asrGenNames(GLsizei count, GLuint* names)
{
    for (GLsizei i = 0; i < count; ++i)
    {
        names[i] = sNextName.fetch_add(1, std::memory_order_relaxed);
    }
}

GLuint APIENTRY asrCreateProgram()
{
    return sNextName.fetch_add(1, std::memory_order_relaxed);
}

GLuint APIENTRY asrCreateShader(GLenum)
{
    return sNextName.fetch_add(1, std::memory_order_relaxed);
}

void APIENTRY asrGetShaderiv(GLuint, GLenum name, GLint* value)
{
    *value = name == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

void APIENTRY asrGetProgramiv(GLuint, GLenum name, GLint* value)
{
    *value = (name == GL_LINK_STATUS || name == GL_VALIDATE_STATUS) ? GL_TRUE : 0;
}

GLint APIENTRY asrGetLocation(GLuint, const GLchar*)
{
    return -1;
}

GLuint APIENTRY asrGetUniformBlockIndex(GLuint, const GLchar*)
{
    return GL_INVALID_INDEX;
}

GLenum APIENTRY asrCheckFramebufferStatus(GLenum)
{
    return GL_FRAMEBUFFER_COMPLETE;
}

GLsync APIENTRY asrFenceSync(GLenum, GLbitfield)
{
    return reinterpret_cast<GLsync>(uintptr_t(1));
}

GLenum APIENTRY asrClientWaitSync(GLsync, GLbitfield, GLuint64)
{
    return GL_ALREADY_SIGNALED;
}

void APIENTRY asrGetQueryObject32(GLuint, GLenum, GLuint* value)
{
    *value = 1;
}

void APIENTRY asrGetQueryObject64(GLuint, GLenum, GLuint64* value)
{
    *value = 1;
}

// --- Stubs --------------------------------------------------------------------

// Every GL function the viewer loads gets its own stub, so each can log its own
// name. The stub takes no arguments and returns 0, which is safe for any GL
// signature on these platforms: the caller cleans up its own arguments.
constexpr std::size_t MAX_STUBS = 1024;

using Stub = void* (APIENTRY*)();

const char* sStubNames[MAX_STUBS] = {};
std::atomic<bool> sStubCalled[MAX_STUBS];
std::size_t sStubCount = 0;

void reportFirstCall(std::size_t index)
{
    if (!sStubCalled[index].exchange(true, std::memory_order_relaxed))
    {
        LL_INFOS("ASRGLTable") << "stub reached: " << sStubNames[index] << LL_ENDL;
    }
}

template <std::size_t INDEX>
void* APIENTRY stub()
{
    if (!sStubCalled[INDEX].load(std::memory_order_relaxed))
    {
        reportFirstCall(INDEX);
    }
    return nullptr;
}

template <std::size_t... INDICES>
const Stub* stubArray(std::index_sequence<INDICES...>)
{
    static const Stub stubs[] = { &stub<INDICES>... };
    return stubs;
}

void* nextStub(const char* name)
{
    if (sStubCount == MAX_STUBS)
    {
        LL_ERRS("ASRGLTable") << "out of stubs at " << name << ": raise MAX_STUBS" << LL_ENDL;
    }
    sStubNames[sStubCount] = name;
    return reinterpret_cast<void*>(stubArray(std::make_index_sequence<MAX_STUBS>())[sStubCount++]);
}

template <class FUNCTION>
void* answer(FUNCTION function)
{
    return reinterpret_cast<void*>(function);
}

} // namespace

bool ASR::active()
{
    return sActive;
}

void ASR::setActive(const std::string& vendor, const std::string& renderer)
{
    sVendor = vendor;
    sRenderer = renderer;
    sActive = true;
}

void* ASRGLTable::get(const char* name)
{
    static std::unordered_map<std::string, void*> table = {
        { "glGetString",               answer(asrGetString) },
        { "glGetStringi",              answer(asrGetStringi) },
        { "glGetIntegerv",             answer(asrGetIntegerv) },
        { "glGetBooleanv",             answer(asrGetBooleanv) },
        { "glGetFloatv",               answer(asrGetFloatv) },
        { "glGetDoublev",              answer(asrGetDoublev) },
        { "glGenTextures",             answer(asrGenNames) },
        { "glGenBuffers",              answer(asrGenNames) },
        { "glGenFramebuffers",         answer(asrGenNames) },
        { "glGenRenderbuffers",        answer(asrGenNames) },
        { "glGenVertexArrays",         answer(asrGenNames) },
        { "glGenQueries",              answer(asrGenNames) },
        { "glGenSamplers",             answer(asrGenNames) },
        { "glCreateProgram",           answer(asrCreateProgram) },
        { "glCreateShader",            answer(asrCreateShader) },
        { "glGetShaderiv",             answer(asrGetShaderiv) },
        { "glGetProgramiv",            answer(asrGetProgramiv) },
        { "glGetUniformLocation",      answer(asrGetLocation) },
        { "glGetAttribLocation",       answer(asrGetLocation) },
        { "glGetUniformBlockIndex",    answer(asrGetUniformBlockIndex) },
        { "glCheckFramebufferStatus",  answer(asrCheckFramebufferStatus) },
        { "glFenceSync",               answer(asrFenceSync) },
        { "glClientWaitSync",          answer(asrClientWaitSync) },
        { "glGetQueryObjectiv",        answer(asrGetQueryObject32) },
        { "glGetQueryObjectuiv",       answer(asrGetQueryObject32) },
        { "glGetQueryObjecti64v",      answer(asrGetQueryObject64) },
        { "glGetQueryObjectui64v",     answer(asrGetQueryObject64) },
    };

    const auto found = table.find(name);
    if (found != table.end())
    {
        return found->second;
    }

    // Asked again after a window rebuild: the same name keeps the same stub.
    void* function = nextStub(name);
    table.emplace(name, function);
    return function;
}
