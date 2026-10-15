#pragma once

namespace vrc {

class AppState;

// Builds one frame of the interface from `app`. Everything ImGui and every GPU texture
// used by the previews lives behind this call; main.cpp only runs the sokol_app loop.
// Called between simgui_new_frame() and simgui_render().
void ui_draw(AppState* app);

// Opens whatever folder dialog the last ui_draw() recorded, and applies the answer.
//
// This MUST be called with no ImGui frame open, after the frame has been flushed and
// drawn. A native dialog runs a nested event loop: the display link keeps firing, the
// frame callback is re-entered, and calling ImGui::NewFrame() in the middle of an
// unfinished frame corrupts its window stack. That crash is what this separation exists
// to prevent, so do not move this call back inside the drawing.
void ui_handle_pending_dialogs(AppState* app);

// Releases the preview textures. Called before simgui_shutdown().
void ui_discard_textures();

}  // namespace vrc
