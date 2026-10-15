#pragma once

namespace vrc {

class AppState;

// Builds one frame of the interface from `app`. Everything ImGui and every GPU texture
// used by the previews lives behind this call; main.cpp only runs the sokol_app loop.
// Called between simgui_new_frame() and simgui_render().
void ui_draw(AppState* app);

// Releases the preview textures. Called before simgui_shutdown().
void ui_discard_textures();

}  // namespace vrc
