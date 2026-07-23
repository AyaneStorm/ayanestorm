/**
 * @file fsavboit.h
 * @brief Approximate adaptive voxel-based OIT resolve for Exact OIT captures.
 * @author chanayane@firestorm
 */

#ifndef FS_AVBOIT_H
#define FS_AVBOIT_H

#include "llgl.h"

#include <vector>

class LLGLSLShader;
class LLRenderTarget;
class LLSD;

class FSAVBOIT
{
public:
    static bool requested();
    static void loadShaders(S32 shader_level);
    static void registerShaders(std::vector<LLGLSLShader*>& shader_list);
    static void unloadShaders();
    static void allocateResources(U32 width, U32 height);
    static void releaseResources();
    static void appendDiagnostics(LLSD& info);
    static bool composite(LLRenderTarget& screen, LLRenderTarget& opaque,
                          GLuint heads, GLuint nodes, U32 width, U32 height);

private:
    static bool supported();
    static bool available();
    static bool allocateVolume(U32 width, U32 height);

    struct Resources
    {
        GLuint extinction = 0;
        GLuint transmittance = 0;
        GLuint classification = 0;
        GLuint zeroTransmittanceDepth = 0;
        GLuint occupancy = 0;
        GLuint warp = 0;
        GLuint tileOccupancy = 0;
        U32 volumeWidth = 0;
        U32 volumeHeight = 0;
        bool available = false;
    };
    static Resources sResources;
};

#endif
