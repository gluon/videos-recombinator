#pragma once

// The sokol backend, chosen in one place so every translation unit agrees. Including this
// before any sokol header is what keeps main.cpp and ui_draw.cpp compatible.

#if defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif
