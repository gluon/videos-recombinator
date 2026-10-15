#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/plan.h"
#include "io/decoder.h"
#include "io/encoder.h"
#include "io/frame_cache.h"

// Everything the application decides lives here: no sokol, no ImGui, no ffmpeg header.
// src/app/main.cpp draws this and does nothing else, so the seven steps can be driven and
// checked by a test with no window, no GPU and no movie files.
//
// Threading. A Decoder is single-consumer: frame_at() and prefetch() come from one thread.
// So the preview and the render never share one. The UI thread owns the preview decoders;
// start_render() opens its own set inside the render thread and closes them when it ends.

namespace vrc {

// A folder dialog the user asked for while the interface was being drawn. It is recorded
// rather than opened on the spot: a native modal dialog runs a nested event loop, which
// re-enters the frame callback and therefore ImGui, from inside an unfinished ImGui frame.
// The shell picks this up once the frame is over.
enum class FolderRequest { None, Source, Output };

// The seven steps of the original patch, in order. A step is reachable when the one before
// it is satisfied, which is what the patch's numbering implied without enforcing.
enum class Step {
    PickFolder = 1,   // 1. choose the folder and the extension filter
    PickFiles,        // 2. choose one file per voice (the patch's load and buffer)
    SetCuts,          // 3. segments per source, black frames, longest black run
    Calculate,        // 4. draw the cuts and the order
    Combine,          // 5. redraw the order only, keeping the cuts
    SetOutput,        // 6. output folder, codec, size, rate, scaling
    Render            // 7. write the file
};

struct SourceSlot {
    std::string path;                  // empty when nothing is loaded
    Source info;                       // valid once the decoder opened
    std::string error;                 // last failure for this slot
    std::unique_ptr<Decoder> decoder;  // preview decoder, UI thread only
    int64_t preview_frame = 0;

    bool loaded() const { return decoder != nullptr; }
};

// Where a render has got to. Read by the UI every frame; written by the render thread.
struct RenderProgress {
    bool running = false;
    bool finished = false;
    bool ok = false;
    bool cancelled = false;
    int64_t done = 0;
    int64_t total = 0;
    std::string path;   // the file being written
    std::string error;  // set when ok is false
};

class AppState {
  public:
    AppState();
    ~AppState();

    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    // ---- step 1: the source folder ------------------------------------------------
    // Extension filters, in the order the patch's menu had them.
    static const char* const* filters();  // nullptr-terminated: ".mov", ".jpg", ".mp4", "ALL"
    static int filter_count();

    void set_filter(int index);
    int filter() const { return filter_; }

    // Lists the movies directly inside `folder` that pass the filter. Sets an error and
    // leaves the listing empty when the folder cannot be read.
    void set_source_folder(const std::string& folder);
    const std::string& source_folder() const { return source_folder_; }
    const std::vector<std::string>& folder_files() const { return folder_files_; }
    void rescan_folder();

    // ---- step 2: the voices -------------------------------------------------------
    // The patch had two. This keeps that as the starting point and allows more, because
    // the port encodes the voice in a field rather than in the sign of a frame number.
    static constexpr int kMaxSources = 8;

    int source_count() const { return static_cast<int>(slots_.size()); }
    const SourceSlot& slot(int i) const { return slots_[static_cast<size_t>(i)]; }
    bool add_source();     // false when already at kMaxSources
    bool remove_source();  // false when down to one
    // Opens `path` into slot `i` and probes it. Invalidates any plan, since the sources
    // changed. False on failure, with the reason in slot(i).error.
    bool load_source(int i, const std::string& path);
    void unload_source(int i);
    int loaded_count() const;

    // ---- step 3 and 4: the cuts ---------------------------------------------------
    RecombineParams& params() { return params_; }
    const RecombineParams& params() const { return params_; }

    // Frames the current parameters will produce, before any plan exists. This counts
    // every loaded source, which the patch's own readout did not.
    int64_t predicted_frames() const;

    // Draws new cuts, places the black runs and orders the segments: the patch's
    // CALCULATE. False with an error when the parameters cannot be satisfied.
    bool calculate();

    // ---- step 5: the order only ---------------------------------------------------
    // The patch's second button: keep the cuts, redraw their order.
    bool recombine();

    bool has_plan() const { return plan_.ok; }
    const Plan& plan() const { return plan_.plan; }
    uint64_t digest() const { return digest_; }
    const std::string& plan_error() const { return plan_error_; }

    // Two seeds, because there are two draws. `cut_seed` made the cuts, the black runs and
    // the first order. `order_seed` is set only once Combine has redrawn the order, and
    // both are needed to reproduce what is on screen. command_line() emits both.
    uint64_t cut_seed() const { return cut_seed_; }
    uint64_t order_seed() const { return order_seed_; }
    bool reordered() const { return reordered_; }

    // ---- step 6: the output -------------------------------------------------------
    OutputParams& output() { return out_; }
    const OutputParams& output() const { return out_; }
    void set_output_folder(const std::string& folder);
    const std::string& output_folder() const { return out_folder_; }
    // Fills a zero width, height or rate from the first loaded source.
    void adopt_source_output();

    static const char* const* codecs();  // nullptr-terminated
    static int codec_count();

    // ---- step 7: the render -------------------------------------------------------
    bool start_render();  // false when it cannot start, reason in progress().error
    void cancel_render();
    RenderProgress progress() const;
    // Joins a finished thread. Call once per frame from the UI; cheap when idle.
    void poll_render();

    // ---- previews -----------------------------------------------------------------
    // Decoded frame of one source, or nullptr with the reason in slot(i).error.
    const Frame* source_preview(int i, int64_t frame);
    // The frame the plan plays at `slot_index`: either a source frame or opaque black.
    const Frame* plan_preview(int64_t slot_index, std::string* error);
    int64_t preview_slot() const { return preview_slot_; }
    void set_preview_slot(int64_t i);

    // ---- folder dialogs -----------------------------------------------------------
    // Drawing records the request; the shell runs the dialog after the frame and applies
    // the answer. Never open a modal dialog while an ImGui frame is open.
    void request_folder(FolderRequest r) { folder_request_ = r; }
    FolderRequest take_folder_request() {
        const FolderRequest r = folder_request_;
        folder_request_ = FolderRequest::None;
        return r;
    }

    // ---- cross-cutting ------------------------------------------------------------
    // The furthest step the current state allows the user to act on.
    Step reachable_step() const;
    bool step_enabled(Step s) const;

    // The vrc invocation that reproduces exactly what the UI is about to render. The
    // improvement the patch could not offer: explore here, batch from the shell.
    std::string command_line() const;

  private:
    void invalidate_plan();
    std::vector<Source> sources_for_plan() const;

    int filter_ = 0;
    std::string source_folder_;
    std::vector<std::string> folder_files_;
    std::string folder_error_;

    std::vector<SourceSlot> slots_;

    RecombineParams params_;
    PlanResult plan_;
    uint64_t digest_ = 0;
    uint64_t cut_seed_ = 0;
    uint64_t order_seed_ = 0;
    bool reordered_ = false;
    std::string plan_error_;

    OutputParams out_;
    std::string out_folder_;
    FolderRequest folder_request_ = FolderRequest::None;

    std::unique_ptr<FrameCache> preview_cache_;
    int64_t preview_slot_ = 0;
    Frame preview_black_;

    std::thread render_thread_;
    mutable std::mutex render_mutex_;
    RenderProgress render_;
    std::atomic<bool> render_cancel_{false};
};

}  // namespace vrc
