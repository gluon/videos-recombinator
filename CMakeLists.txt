cmake_minimum_required(VERSION 3.20)
project(videos_recombinator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VRC_BUILD_CLI "Build the vrc command line renderer (needs ffmpeg)" ON)
option(VRC_BUILD_UI  "Build the vrc-ui sokol application (needs sokol)"   ON)
option(VRC_BUILD_TESTS "Build the core tests"                             ON)

# ---------------------------------------------------------------------------
# vrc_core: the planner. Pure C++, no ffmpeg, no sokol, no files. It must stay
# that way: everything that can be tested without a machine lives here.
# ---------------------------------------------------------------------------
add_library(vrc_core STATIC
  src/core/plan.cpp
)
target_include_directories(vrc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(vrc_core PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall -Wextra>
)

# ---------------------------------------------------------------------------
# ffmpeg
# ---------------------------------------------------------------------------
if(VRC_BUILD_CLI)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
      libavformat libavcodec libavutil libswscale)
  endif()
  if(NOT FFMPEG_FOUND)
    message(WARNING
      "ffmpeg development packages not found, skipping vrc. "
      "macOS: brew install ffmpeg pkg-config. "
      "Debian: apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev")
    set(VRC_BUILD_CLI OFF)
  endif()
endif()

if(VRC_BUILD_CLI)
  add_library(vrc_io STATIC
    src/io/decoder.cpp
    src/io/frame_cache.cpp
    src/io/encoder.cpp
  )
  target_include_directories(vrc_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vrc_io PUBLIC vrc_core PkgConfig::FFMPEG)

  add_executable(vrc src/cli/main.cpp)
  target_link_libraries(vrc PRIVATE vrc_core vrc_io)
endif()

# ---------------------------------------------------------------------------
# sokol, header only, fetched at configure time
# ---------------------------------------------------------------------------
if(VRC_BUILD_UI)
  include(FetchContent)
  FetchContent_Declare(sokol
    GIT_REPOSITORY https://github.com/floooh/sokol.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(sokol)

  add_executable(vrc-ui src/app/main.cpp)
  target_include_directories(vrc-ui PRIVATE ${sokol_SOURCE_DIR})
  target_link_libraries(vrc-ui PRIVATE vrc_core)
  if(TARGET vrc_io)
    target_link_libraries(vrc-ui PRIVATE vrc_io)
  endif()

  if(APPLE)
    set_source_files_properties(src/app/main.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")
    target_link_libraries(vrc-ui PRIVATE
      "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit"
      "-framework AudioToolbox")
  elseif(UNIX)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(vrc-ui PRIVATE X11::X11 X11::Xi X11::Xcursor OpenGL::GL dl m pthread)
  endif()
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(VRC_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE vrc_core)
  add_test(NAME core COMMAND test_core)
endif()
