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

# vrc_cache: the frame cache. It talks to Decoder through its header only, so it needs no
# ffmpeg and stays on the ffmpeg-free side of the build with vrc_core.
add_library(vrc_cache STATIC src/io/frame_cache.cpp)
target_link_libraries(vrc_cache PUBLIC vrc_core)

# vrc_app_logic: everything the interface decides. No sokol, no ImGui, no ffmpeg: it uses
# the Decoder and Encoder interfaces, which is what lets test_app substitute fakes for them
# and run with no window and no movie files.
add_library(vrc_app_logic STATIC src/app/app_state.cpp)
target_link_libraries(vrc_app_logic PUBLIC vrc_core vrc_cache)

if(VRC_BUILD_CLI)
  add_library(vrc_io STATIC
    src/io/decoder.cpp
    src/io/encoder.cpp
  )
  target_include_directories(vrc_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vrc_io PUBLIC vrc_cache PkgConfig::FFMPEG)

  add_executable(vrc src/cli/main.cpp)
  # vrc_io already links vrc_cache and vrc_core PUBLIC; naming them again here makes the
  # linker see the same archive twice and warn.
  target_link_libraries(vrc PRIVATE vrc_io)
endif()

# ---------------------------------------------------------------------------
# vrc-ui: sokol for the window and the GPU, Dear ImGui for the widgets.
#
# Both are pinned to exact revisions, not to a branch. sokol's API moves: between the
# bootstrap of this repo and the interface landing, sg_update_image was replaced by
# sg_write_image_transient and ImGui textures became sg_view rather than sg_image. A
# floating GIT_TAG would break the build without anyone touching this repo. Raise these
# deliberately, and read sokol's CHANGELOG.md when you do.
# ---------------------------------------------------------------------------
if(VRC_BUILD_UI)
  include(FetchContent)
  FetchContent_Declare(sokol
    GIT_REPOSITORY https://github.com/floooh/sokol.git
    GIT_TAG        5e94d1291fea504ed911090338cf35bd9ef49a06
  )
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(sokol imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  )
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})

  if(APPLE)
    set(VRC_FILE_DIALOG src/app/file_dialog_mac.mm)
  else()
    set(VRC_FILE_DIALOG src/app/file_dialog_stub.cpp)
  endif()

  add_executable(vrc-ui
    src/app/main.cpp
    src/app/ui_draw.cpp
    ${VRC_FILE_DIALOG}
  )
  target_include_directories(vrc-ui PRIVATE ${sokol_SOURCE_DIR})
  target_link_libraries(vrc-ui PRIVATE vrc_app_logic imgui)
  if(TARGET vrc_io)
    target_link_libraries(vrc-ui PRIVATE vrc_io)
  else()
    message(WARNING
      "vrc-ui is being built without ffmpeg: it will open, and every attempt to load a "
      "movie or render will report that the decoder is unavailable.")
  endif()

  if(APPLE)
    # Only main.cpp holds sokol_app's implementation, which is Objective-C on macOS.
    # ui_draw.cpp is plain C++ and must stay that way.
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

  # The interface's logic, driven with no window, no GPU, no ffmpeg and no movie files:
  # the test defines Decoder::open and Encoder::create itself, so it must link
  # vrc_app_logic without vrc_io. Part of the gate that runs anywhere.
  add_executable(test_app tests/test_app.cpp src/app/file_dialog_stub.cpp)
  target_link_libraries(test_app PRIVATE vrc_app_logic)
  add_test(NAME app COMMAND test_app)

  # Integration: real movies generated by ffmpeg, rendered by vrc, counted by ffprobe.
  # Registered whenever vrc is built. A missing ffmpeg or ffprobe makes it fail, not skip.
  if(VRC_BUILD_CLI)
    add_executable(test_io tests/test_io.cpp)
    target_link_libraries(test_io PRIVATE vrc_io)
    find_program(VRC_FFMPEG ffmpeg)
    find_program(VRC_FFPROBE ffprobe)
    add_test(NAME render
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_render.sh
              $<TARGET_FILE:vrc> $<TARGET_FILE:test_io>
              ${VRC_FFMPEG} ${VRC_FFPROBE} ${CMAKE_CURRENT_BINARY_DIR}/render_test)
  endif()
endif()
