// The interface, drawn with Dear ImGui through sokol_imgui. It reads and drives AppState
// and holds no state of its own beyond the GPU textures the previews need, which is why
// this file can be compiled and checked without a window: it contains no sokol_app
// implementation and no platform code.
//
// The seven numbered steps are the patch's own, in the patch's order.
//
// sokol-gfx API notes, current as of the pinned commit. Both changed recently, so do not
// "fix" them back:
//   - a texture is bound to ImGui through an sg_view, not an sg_image:
//     sg_make_view({.texture.image = img}) then simgui_imtextureid(view).
//   - sg_update_image no longer exists. A texture rewritten every frame is created with
//     usage.write_transient and filled with sg_write_image_transient.

#include "app/ui_draw.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/sokol_config.h"

#include "sokol_app.h"
#include "sokol_gfx.h"

#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "util/sokol_imgui.h"

#include "app/app_state.h"
#include "app/file_dialog.h"

namespace vrc {
namespace {

// One GPU texture plus its view per preview pane, rebuilt when the frame size changes.
struct Preview {
    sg_image image = {};
    sg_view view = {};
    int width = 0;
    int height = 0;
    int64_t shown_frame = -1;
    bool valid = false;
};

Preview g_source_preview[AppState::kMaxSources];
Preview g_plan_preview;
char g_source_folder[1024] = {0};
char g_output_folder[1024] = {0};
std::vector<uint8_t> g_packed;

void release(Preview* p) {
    if (p->view.id != SG_INVALID_ID) sg_destroy_view(p->view);
    if (p->image.id != SG_INVALID_ID) sg_destroy_image(p->image);
    *p = Preview{};
}

// Uploads an RGBA frame. sg_write_image_transient takes a tightly packed source, so a
// frame whose stride exceeds its width is repacked first.
void upload(Preview* p, const Frame& f) {
    if (p->width != f.width || p->height != f.height) {
        release(p);
        sg_image_desc id = {};
        id.width = f.width;
        id.height = f.height;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.usage.write_transient = true;
        id.label = "vrc-preview";
        p->image = sg_make_image(&id);

        sg_view_desc vd = {};
        vd.texture.image = p->image;
        vd.label = "vrc-preview-view";
        p->view = sg_make_view(&vd);

        p->width = f.width;
        p->height = f.height;
    }

    const int tight = f.width * 4;
    const uint8_t* src = f.pixels.data();
    if (f.stride != tight) {
        g_packed.resize(static_cast<size_t>(tight) * f.height);
        for (int y = 0; y < f.height; ++y)
            std::memcpy(g_packed.data() + static_cast<size_t>(y) * tight,
                        f.pixels.data() + static_cast<size_t>(y) * f.stride,
                        static_cast<size_t>(tight));
        src = g_packed.data();
    }

    sg_write_image_desc wd = {};
    wd.src.data.ptr = src;
    wd.src.data.size = static_cast<size_t>(tight) * f.height;
    wd.dst.image = p->image;
    wd.size.width = f.width;
    wd.size.height = f.height;
    wd.size.num_slices = 1;
    sg_write_image_transient(&wd);
    p->valid = true;
}

void draw_preview(const Preview& p, float width) {
    if (!p.valid || p.width <= 0) {
        ImGui::Dummy(ImVec2(width, width * 9.0f / 16.0f));
        return;
    }
    const float h = width * static_cast<float>(p.height) / static_cast<float>(p.width);
    ImGui::Image(simgui_imtextureid(p.view), ImVec2(width, h));
}

void step_header(int n, const char* label, bool enabled) {
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? ImVec4(1.0f, 0.94f, 0.33f, 1.0f)
                                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::Text("%d", n);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::Separator();
}

void draw_sources_panel(AppState& a) {
    step_header(1, "SOURCE FOLDER", true);

    if (has_native_dialog()) {
        // Recorded, not opened: a modal dialog here would re-enter this very frame.
        if (ImGui::Button("Choose folder...")) a.request_folder(FolderRequest::Source);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##srcfolder", g_source_folder, sizeof g_source_folder,
                         ImGuiInputTextFlags_EnterReturnsTrue))
        a.set_source_folder(g_source_folder);

    int filter = a.filter();
    for (int i = 0; i < AppState::filter_count(); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(AppState::filters()[i], &filter, i)) a.set_filter(i);
    }
    ImGui::TextDisabled("%zu file(s) match", a.folder_files().size());

    ImGui::Dummy(ImVec2(0, 8));
    step_header(2, "VOICES", a.step_enabled(Step::PickFiles));

    if (ImGui::SmallButton("+ voice")) a.add_source();
    ImGui::SameLine();
    if (ImGui::SmallButton("- voice")) a.remove_source();
    ImGui::SameLine();
    ImGui::TextDisabled("%d loaded of %d", a.loaded_count(), a.source_count());

    for (int i = 0; i < a.source_count(); ++i) {
        ImGui::PushID(i);
        ImGui::Text("voice %d", i + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60.0f);
        const char* current = a.slot(i).loaded() ? a.slot(i).path.c_str() : "(none)";
        if (ImGui::BeginCombo("##file", current)) {
            for (const auto& name : a.folder_files()) {
                if (ImGui::Selectable(name.c_str())) {
                    a.load_source(i, a.source_folder() + "/" + name);
                    release(&g_source_preview[i]);
                    release(&g_plan_preview);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            a.unload_source(i);
            release(&g_source_preview[i]);
            release(&g_plan_preview);
        }
        if (a.slot(i).loaded()) {
            const Source& s = a.slot(i).info;
            ImGui::TextDisabled("  %lld frames, %dx%d, %.3f fps",
                                static_cast<long long>(s.frame_count), s.width, s.height,
                                s.fps);
        } else if (!a.slot(i).error.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "  %s", a.slot(i).error.c_str());
        }
        ImGui::PopID();
    }
}

void draw_cuts_panel(AppState& a) {
    RecombineParams& p = a.params();

    ImGui::Dummy(ImVec2(0, 8));
    step_header(3, "CUTS", a.step_enabled(Step::SetCuts));

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("segments per voice", &p.segments_per_source))
        if (p.segments_per_source < 1) p.segments_per_source = 1;
    int black = static_cast<int>(p.black_total);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("black frames", &black)) p.black_total = black < 0 ? 0 : black;
    int run = static_cast<int>(p.black_run_max);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt("longest black run", &run)) p.black_run_max = run < 1 ? 1 : run;
    ImGui::Checkbox("avoid adjacent segments of one voice", &p.avoid_adjacent_segments);
    ImGui::TextDisabled("predicted output: %lld frames",
                        static_cast<long long>(a.predicted_frames()));

    ImGui::Dummy(ImVec2(0, 8));
    step_header(4, "CALCULATE", a.step_enabled(Step::Calculate));
    ImGui::BeginDisabled(!a.step_enabled(Step::Calculate));
    if (ImGui::Button("CALCULATE", ImVec2(-1.0f, 40.0f)))
        if (a.calculate()) release(&g_plan_preview);
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 8));
    step_header(5, "COMBINE AGAIN", a.step_enabled(Step::Combine));
    ImGui::BeginDisabled(!a.step_enabled(Step::Combine));
    if (ImGui::Button("new order, same cuts", ImVec2(-1.0f, 30.0f)))
        if (a.recombine()) release(&g_plan_preview);
    ImGui::EndDisabled();

    if (!a.plan_error().empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", a.plan_error().c_str());

    if (a.has_plan()) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::Text("segments  %zu", a.plan().segments.size());
        ImGui::Text("frames    %zu", a.plan().slots.size());
        ImGui::Text("seed      %" PRIu64, a.cut_seed());
        if (a.reordered()) ImGui::Text("reorder   %" PRIu64, a.order_seed());
        ImGui::Text("plan      %016" PRIx64, a.digest());
    }
}

void draw_output_panel(AppState& a) {
    OutputParams& o = a.output();

    ImGui::Dummy(ImVec2(0, 8));
    step_header(6, "OUTPUT", a.step_enabled(Step::SetOutput));

    if (has_native_dialog()) {
        if (ImGui::Button("Choose output folder...")) a.request_folder(FolderRequest::Output);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outfolder", g_output_folder, sizeof g_output_folder,
                         ImGuiInputTextFlags_EnterReturnsTrue))
        a.set_output_folder(g_output_folder);

    int codec = 0;
    for (int i = 0; i < AppState::codec_count(); ++i)
        if (o.codec == AppState::codecs()[i]) codec = i;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("codec", &codec, AppState::codecs(), AppState::codec_count()))
        o.codec = AppState::codecs()[codec];

    // Scaling's enumerators are Stretch, Fit, Fill, in that order.
    const char* modes[] = {"stretch", "fit", "fill"};
    int scaling = static_cast<int>(o.scaling);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("scaling", &scaling, modes, 3))
        o.scaling = static_cast<Scaling>(scaling);

    int wh[2] = {o.width, o.height};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputInt2("size", wh)) {
        o.width = wh[0] < 0 ? 0 : wh[0];
        o.height = wh[1] < 0 ? 0 : wh[1];
    }
    float fps = static_cast<float>(o.fps);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputFloat("fps", &fps, 0.0f, 0.0f, "%.3f"))
        o.fps = fps < 0.0f ? 0.0 : static_cast<double>(fps);
    ImGui::SameLine();
    if (ImGui::SmallButton("from voice 1")) {
        o.width = o.height = 0;
        o.fps = 0.0;
        a.adopt_source_output();
    }
    ImGui::TextDisabled("zero means: take it from the first loaded voice");
}

void draw_render_panel(AppState& a) {
    const RenderProgress pr = a.progress();

    ImGui::Dummy(ImVec2(0, 8));
    step_header(7, "RENDER", a.step_enabled(Step::Render));

    ImGui::BeginDisabled(!a.step_enabled(Step::Render) || pr.running);
    if (ImGui::Button("RENDER", ImVec2(-1.0f, 40.0f))) a.start_render();
    ImGui::EndDisabled();

    if (pr.running) {
        const float f = pr.total > 0 ? static_cast<float>(pr.done) /
                                           static_cast<float>(pr.total)
                                     : 0.0f;
        char label[64];
        std::snprintf(label, sizeof label, "%lld / %lld", static_cast<long long>(pr.done),
                      static_cast<long long>(pr.total));
        ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f), label);
        if (ImGui::Button("cancel")) a.cancel_render();
        ImGui::TextDisabled("%s", pr.path.c_str());
    } else if (pr.finished) {
        if (pr.ok)
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "wrote %s", pr.path.c_str());
        else if (pr.cancelled)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                               "cancelled, %s is incomplete and unplayable",
                               pr.path.c_str());
        else
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", pr.error.c_str());
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("the same thing from the shell");
    const std::string cmd = a.command_line();
    ImGui::TextWrapped("%s", cmd.c_str());
    if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(cmd.c_str());
}

void draw_previews(AppState& a) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float half = (w - 8.0f) * 0.5f;

    ImGui::SeparatorText("voices");
    int drawn = 0;
    for (int i = 0; i < a.source_count(); ++i) {
        if (!a.slot(i).loaded()) continue;
        if (drawn % 2) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(100 + i);

        const int64_t frame = a.slot(i).preview_frame;
        const Frame* f = a.source_preview(i, frame);
        if (f != nullptr && g_source_preview[i].shown_frame != frame) {
            upload(&g_source_preview[i], *f);
            g_source_preview[i].shown_frame = frame;
        }
        draw_preview(g_source_preview[i], half);

        ImGui::SetNextItemWidth(half);
        int pos = static_cast<int>(frame);
        const int last = static_cast<int>(a.slot(i).info.frame_count) - 1;
        if (ImGui::SliderInt("##scrub", &pos, 0, last > 0 ? last : 0))
            a.source_preview(i, pos);

        ImGui::PopID();
        ImGui::EndGroup();
        ++drawn;
    }
    if (drawn == 0) ImGui::TextDisabled("load a voice to see it");

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("output");
    if (!a.has_plan()) {
        ImGui::TextDisabled("calculate a plan to scrub through it");
        return;
    }

    const int64_t n = static_cast<int64_t>(a.plan().slots.size());
    int pos = static_cast<int>(a.preview_slot());
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("##planscrub", &pos, 0, static_cast<int>(n) - 1))
        a.set_preview_slot(pos);

    std::string err;
    const Frame* f = a.plan_preview(a.preview_slot(), &err);
    if (f != nullptr && g_plan_preview.shown_frame != a.preview_slot()) {
        upload(&g_plan_preview, *f);
        g_plan_preview.shown_frame = a.preview_slot();
    }
    draw_preview(g_plan_preview, w);
    if (f == nullptr && !err.empty())
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", err.c_str());

    const Slot& s = a.plan().slots[static_cast<size_t>(a.preview_slot())];
    if (s.kind == SlotKind::Black)
        ImGui::Text("slot %lld / %lld: black", static_cast<long long>(a.preview_slot()),
                    static_cast<long long>(n - 1));
    else
        ImGui::Text("slot %lld / %lld: voice %d, frame %lld",
                    static_cast<long long>(a.preview_slot()),
                    static_cast<long long>(n - 1), s.source_index + 1,
                    static_cast<long long>(s.frame));
}

}  // namespace

void ui_handle_pending_dialogs(AppState* app) {
    AppState& a = *app;
    const FolderRequest r = a.take_folder_request();
    if (r == FolderRequest::None) return;

    std::string err;
    const std::string folder =
        pick_folder(r == FolderRequest::Source ? "Folder holding the source movies"
                                               : "Where to write the movie",
                    &err);
    if (folder.empty()) return;  // cancelled, or no dialog on this platform

    if (r == FolderRequest::Source) {
        a.set_source_folder(folder);
        std::snprintf(g_source_folder, sizeof g_source_folder, "%s", folder.c_str());
    } else {
        a.set_output_folder(folder);
        std::snprintf(g_output_folder, sizeof g_output_folder, "%s", folder.c_str());
    }
}

void ui_discard_textures() {
    for (int i = 0; i < AppState::kMaxSources; ++i) release(&g_source_preview[i]);
    release(&g_plan_preview);
    g_packed.clear();
    g_packed.shrink_to_fit();
}

void ui_draw(AppState* app) {
    AppState& a = *app;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("videos recombinator", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float left = vp->WorkSize.x * 0.42f;
    ImGui::BeginChild("controls", ImVec2(left, 0), ImGuiChildFlags_Borders);
    draw_sources_panel(a);
    draw_cuts_panel(a);
    draw_output_panel(a);
    draw_render_panel(a);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("previews", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_previews(a);
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace vrc
