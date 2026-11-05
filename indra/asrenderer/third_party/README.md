# Third-party code for the AyaneStorm renderer

Unmodified copies of upstream source, pinned so every platform builds the same code. Only source and licence files are kept: no tests, samples or scripts. To update, replace the files and change the version here. See `doc/ayanestorm-new-renderer-plan.md`.

| Folder | Project | Version | Licence | Purpose |
|---|---|---|---|---|
| `volk` | https://github.com/zeux/volk | `vulkan-sdk-1.4.363.0` | MIT | Loads Vulkan at runtime, so the viewer starts without it and can fall back to OpenGL |
| `vma` | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | `v3.4.0` | MIT | Video memory allocation under `ASRMemoryBudget` |
| `spirv-cross` | https://github.com/KhronosGroup/SPIRV-Cross | `vulkan-sdk-1.4.363.0` | Apache-2.0 | Build-time conversion of SPIR-V to Metal shaders |
| `vulkan-headers` | https://github.com/KhronosGroup/Vulkan-Headers | `vulkan-sdk-1.4.363.0` | Apache-2.0 and MIT | Vulkan's C header files, with no dependency on an installed SDK |
