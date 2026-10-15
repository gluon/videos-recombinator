// vrc-ui: the sokol_app shell. It runs the window and the frame loop and nothing else.
// Every decision, every draw of the dice and every file touched lives in app_state.cpp,
// and the interface itself in ui_draw.cpp, which is why both can be built and checked
// without a window.

#include "app/sokol_config.h"
#define SOKOL_IMPL

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

// SOKOL_IMPL switches on the implementation of every sokol header that follows, and
// sokol_imgui.h is one of them. Its implementation needs imgui.h and belongs in exactly
// one translation unit, which is ui_draw.cpp. Undefining it here keeps this include to
// the declarations.
#undef SOKOL_IMPL
#include "util/sokol_imgui.h"

#include "app/app_state.h"
#include "app/ui_draw.h"

namespace {

vrc::AppState* g_app = nullptr;
sg_pass_action g_pass_action;

void init() {
    sg_desc desc = {};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    sg_setup(&desc);

    simgui_desc_t imgui_desc = {};
    imgui_desc.logger.func = slog_func;
    simgui_setup(&imgui_desc);

    g_pass_action = {};
    g_pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g_pass_action.colors[0].clear_value = {0.08f, 0.08f, 0.09f, 1.0f};

    g_app = new vrc::AppState();
}

void frame() {
    g_app->poll_render();

    simgui_frame_desc_t fd = {};
    fd.width = sapp_width();
    fd.height = sapp_height();
    fd.delta_time = sapp_frame_duration();
    fd.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&fd);

    vrc::ui_draw(g_app);

    // simgui_flush() builds the command lists and must run outside the pass;
    // simgui_draw() issues them and must run inside it.
    simgui_flush();

    sg_pass pass = {};
    pass.action = g_pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    simgui_draw();
    sg_end_pass();
    sg_commit();
}

void cleanup() {
    // The state first: its destructor stops a running render, which is still touching
    // decoders and the output file.
    delete g_app;
    g_app = nullptr;
    vrc::ui_discard_textures();
    simgui_shutdown();
    sg_shutdown();
}

void event(const sapp_event* e) { simgui_handle_event(e); }

}  // namespace

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    sapp_desc desc = {};
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.cleanup_cb = cleanup;
    desc.event_cb = event;
    desc.width = 1400;
    desc.height = 900;
    desc.window_title = "videos recombinator";
    desc.high_dpi = true;
    desc.logger.func = slog_func;
    return desc;
}
