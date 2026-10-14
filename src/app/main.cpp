// vrc-ui: sokol shell. Owns no algorithm. Everything it shows comes from vrc_core and
// vrc_io, so that a plan built here and a plan built by vrc from the same seed and params
// are the same plan.
//
// NOT IMPLEMENTED beyond the window. docs/SPEC.md section 9 says what belongs here.

#define SOKOL_IMPL
#if defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"

#include "core/plan.h"

static sg_pass_action g_pass_action;

static void init() {
    sg_desc desc = {};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    sg_setup(&desc);

    g_pass_action = {};
    g_pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g_pass_action.colors[0].clear_value = {0.08f, 0.08f, 0.09f, 1.0f};
}

static void frame() {
    sg_pass pass = {};
    pass.action = g_pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    sg_end_pass();
    sg_commit();
}

static void cleanup() { sg_shutdown(); }

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    sapp_desc desc = {};
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.cleanup_cb = cleanup;
    desc.width = 1280;
    desc.height = 800;
    desc.window_title = "videos recombinator";
    desc.logger.func = slog_func;
    return desc;
}
