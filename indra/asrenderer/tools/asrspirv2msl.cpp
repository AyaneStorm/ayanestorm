/**
 * asrspirv2msl.cpp
 * Build-time tool: converts one SPIR-V file to one Metal shader source file with
 * the vendored SPIRV-Cross. Usage: asr_spirv2msl input.spv output.metal
 */
#include "spirv_msl.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: asr_spirv2msl input.spv output.metal\n");
        return 2;
    }

    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in)
    {
        fprintf(stderr, "asr_spirv2msl: cannot read %s\n", argv[1]);
        return 1;
    }
    const std::streamsize size = in.tellg();
    if (size <= 0 || size % sizeof(uint32_t) != 0)
    {
        fprintf(stderr, "asr_spirv2msl: %s is not SPIR-V\n", argv[1]);
        return 1;
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()), size);

    try
    {
        spirv_cross::CompilerMSL compiler(std::move(words));

        spirv_cross::CompilerMSL::Options msl;
        msl.set_msl_version(3, 0);
        msl.argument_buffers = true;
        compiler.set_msl_options(msl);

        // Vulkan's clip space has Y down and Metal's has Y up. Flipping here
        // keeps one GLSL source correct on both.
        spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
        common.vertex.flip_vert_y = true;
        compiler.set_common_options(common);

        const std::string source = compiler.compile();
        std::ofstream out(argv[2], std::ios::binary);
        out << source;
        if (!out)
        {
            fprintf(stderr, "asr_spirv2msl: cannot write %s\n", argv[2]);
            return 1;
        }
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "asr_spirv2msl: %s: %s\n", argv[1], error.what());
        return 1;
    }
    return 0;
}
