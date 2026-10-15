#pragma once

// The sokol backend, chosen in one place so every translation unit agrees. Including this
// before any sokol header is what keeps main.cpp and ui_draw.cpp compatible.
//
// A backend defined on the command line wins, which is how a build that only needs to
// check the code can ask for SOKOL_DUMMY_BACKEND.

#if !defined(SOKOL_METAL) && !defined(SOKOL_D3D11) && !defined(SOKOL_GLCORE) && \
    !defined(SOKOL_GLES3) && !defined(SOKOL_WGPU) && !defined(SOKOL_VULKAN) && \
    !defined(SOKOL_DUMMY_BACKEND)
#if defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif
#endif
