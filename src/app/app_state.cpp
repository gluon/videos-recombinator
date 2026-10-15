#include "app/app_state.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace vrc {
namespace {

constexpr size_t kPreviewCacheBytes = 64u * 1024 * 1024;

const char* const kFilters[] = {".mov", ".jpg", ".mp4", "ALL", nullptr};
const char* const kCodecs[] = {"prores422", "prores4444", "h264", "mjpeg", nullptr};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_extension(const std::string& name, const std::string& ext) {
    if (name.size() < ext.size()) return false;
    return lower(name.substr(name.size() - ext.size())) == lower(ext);
}

std::string quoted(const std::string& s) {
    // Good enough for a command line the user copies into a POSIX shell.
    if (s.find_first_of(" \t'\"\\$`()[]{}*?&;|<>!#") == std::string::npos) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

const char* scaling_name(Scaling s) {
    switch (s) {
        case Scaling::Stretch: return "stretch";
        case Scaling::Fill: return "fill";
        case Scaling::Fit: break;
    }
    return "fit";
}

}  // namespace

AppState::AppState() {
    slots_.resize(2);  // the patch's two voices
    params_.segments_per_source = 8;
    params_.black_total = 0;
    params_.black_run_max = 1;
    params_.avoid_adjacent_segments = true;
    params_.seed = 0;
    out_.codec = "prores422";
    out_.scaling = Scaling::Fit;
    preview_cache_ = std::make_unique<FrameCache>(kPreviewCacheBytes);
}

AppState::~AppState() {
    cancel_render();
    if (render_thread_.joinable()) render_thread_.join();
}

const char* const* AppState::filters() { return kFilters; }
int AppState::filter_count() { return 4; }
const char* const* AppState::codecs() { return kCodecs; }
int AppState::codec_count() { return 4; }

void AppState::set_filter(int index) {
    if (index < 0 || index >= filter_count()) return;
    filter_ = index;
    rescan_folder();
}

void AppState::set_source_folder(const std::string& folder) {
    source_folder_ = folder;
    rescan_folder();
}

void AppState::rescan_folder() {
    namespace fs = std::filesystem;
    folder_files_.clear();
    folder_error_.clear();
    if (source_folder_.empty()) return;

    std::error_code ec;
    if (!fs::is_directory(source_folder_, ec)) {
        folder_error_ = source_folder_ + " is not a directory";
        return;
    }
    const std::string ext = kFilters[filter_];
    const bool all = ext == std::string("ALL");
    for (const auto& e : fs::directory_iterator(source_folder_, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;  // resource forks and the like
        if (all || has_extension(name, ext)) folder_files_.push_back(name);
    }
    if (ec) folder_error_ = "cannot read " + source_folder_ + ": " + ec.message();
    std::sort(folder_files_.begin(), folder_files_.end());
}

bool AppState::add_source() {
    if (static_cast<int>(slots_.size()) >= kMaxSources) return false;
    slots_.emplace_back();
    invalidate_plan();
    return true;
}

bool AppState::remove_source() {
    if (slots_.size() <= 1) return false;
    slots_.pop_back();
    invalidate_plan();
    return true;
}

bool AppState::load_source(int i, const std::string& path) {
    if (i < 0 || i >= source_count()) return false;
    SourceSlot& s = slots_[static_cast<size_t>(i)];
    std::string err;
    auto d = Decoder::open(path, &err);
    if (!d) {
        s.error = err;
        return false;
    }
    s.decoder = std::move(d);
    s.info = s.decoder->source();
    s.path = path;
    s.error.clear();
    s.preview_frame = 0;
    preview_cache_ = std::make_unique<FrameCache>(kPreviewCacheBytes);
    for (int k = 0; k < source_count(); ++k)
        if (slots_[static_cast<size_t>(k)].decoder)
            preview_cache_->attach(k, slots_[static_cast<size_t>(k)].decoder.get());
    invalidate_plan();
    return true;
}

void AppState::unload_source(int i) {
    if (i < 0 || i >= source_count()) return;
    slots_[static_cast<size_t>(i)] = SourceSlot{};
    preview_cache_ = std::make_unique<FrameCache>(kPreviewCacheBytes);
    for (int k = 0; k < source_count(); ++k)
        if (slots_[static_cast<size_t>(k)].decoder)
            preview_cache_->attach(k, slots_[static_cast<size_t>(k)].decoder.get());
    invalidate_plan();
}

int AppState::loaded_count() const {
    int n = 0;
    for (const auto& s : slots_)
        if (s.decoder) ++n;
    return n;
}

std::vector<Source> AppState::sources_for_plan() const {
    std::vector<Source> out;
    for (const auto& s : slots_)
        if (s.decoder) out.push_back(s.info);
    return out;
}

int64_t AppState::predicted_frames() const {
    const auto s = sources_for_plan();
    if (s.empty()) return 0;
    return predicted_length(s, params_);
}

void AppState::invalidate_plan() {
    plan_ = PlanResult{};
    digest_ = 0;
    cut_seed_ = 0;
    order_seed_ = 0;
    reordered_ = false;
    plan_error_.clear();
    preview_slot_ = 0;
}

bool AppState::calculate() {
    const auto sources = sources_for_plan();
    if (sources.empty()) {
        plan_error_ = "load at least one source first";
        return false;
    }
    plan_ = build_plan(sources, params_);
    if (!plan_.ok) {
        plan_error_ = plan_.error;
        digest_ = 0;
        return false;
    }
    plan_error_.clear();
    digest_ = plan_digest(plan_.plan);
    cut_seed_ = plan_.seed_used;
    order_seed_ = 0;
    reordered_ = false;
    preview_slot_ = 0;
    return true;
}

bool AppState::recombine() {
    if (!plan_.ok) {
        plan_error_ = "nothing to recombine: calculate first";
        return false;
    }
    // A fresh order every press, keeping the cuts and their black runs. The seed that
    // produced this order is kept alongside the cut seed, so the pair still describes
    // exactly what is on screen.
    auto next = reorder_plan(plan_.plan, 0, params_.avoid_adjacent_segments);
    if (!next.ok) {
        plan_error_ = next.error;
        return false;
    }
    const uint64_t used = next.seed_used;
    plan_ = std::move(next);
    plan_error_.clear();
    digest_ = plan_digest(plan_.plan);
    order_seed_ = used;
    reordered_ = true;
    preview_slot_ = std::min(preview_slot_,
                             static_cast<int64_t>(plan_.plan.slots.size()) - 1);
    return true;
}

void AppState::set_output_folder(const std::string& folder) { out_folder_ = folder; }

void AppState::adopt_source_output() {
    for (const auto& s : slots_) {
        if (!s.decoder) continue;
        if (out_.width == 0) out_.width = s.info.width;
        if (out_.height == 0) out_.height = s.info.height;
        if (out_.fps == 0.0) out_.fps = s.info.fps;
        return;
    }
}

Step AppState::reachable_step() const {
    if (source_folder_.empty() || folder_files_.empty()) return Step::PickFolder;
    if (loaded_count() == 0) return Step::PickFiles;
    if (!plan_.ok) return Step::SetCuts;
    if (out_folder_.empty()) return Step::SetOutput;
    return Step::Render;
}

bool AppState::step_enabled(Step s) const {
    switch (s) {
        case Step::PickFolder: return true;
        case Step::PickFiles: return !folder_files_.empty();
        case Step::SetCuts: return loaded_count() > 0;
        case Step::Calculate: return loaded_count() > 0;
        case Step::Combine: return plan_.ok;
        case Step::SetOutput: return plan_.ok;
        case Step::Render: return plan_.ok && !out_folder_.empty();
    }
    return false;
}

const Frame* AppState::source_preview(int i, int64_t frame) {
    if (i < 0 || i >= source_count()) return nullptr;
    SourceSlot& s = slots_[static_cast<size_t>(i)];
    if (!s.decoder) return nullptr;
    if (frame < 0) frame = 0;
    if (frame >= s.info.frame_count) frame = s.info.frame_count - 1;
    s.preview_frame = frame;
    std::string err;
    const Frame* f = preview_cache_->get(i, frame, &err);
    if (f == nullptr) s.error = err;
    return f;
}

const Frame* AppState::plan_preview(int64_t slot_index, std::string* error) {
    if (!plan_.ok) {
        if (error) *error = "no plan yet";
        return nullptr;
    }
    const auto& slots = plan_.plan.slots;
    if (slot_index < 0 || slot_index >= static_cast<int64_t>(slots.size())) {
        if (error) *error = "slot out of range";
        return nullptr;
    }
    const Slot& s = slots[static_cast<size_t>(slot_index)];
    if (s.kind == SlotKind::Black) {
        // The size the output will have, so the preview shows the real black frame.
        int w = out_.width, h = out_.height;
        if (w <= 0 || h <= 0) {
            for (const auto& sl : slots_)
                if (sl.decoder) {
                    w = sl.info.width;
                    h = sl.info.height;
                    break;
                }
        }
        if (w <= 0 || h <= 0) {
            if (error) *error = "no output size yet";
            return nullptr;
        }
        preview_black_ = preview_cache_->black(w, h);
        return &preview_black_;
    }
    // The plan indexes the loaded sources in order, which is how build_plan saw them.
    int slot_of_source = -1, seen = 0;
    for (int k = 0; k < source_count(); ++k) {
        if (!slots_[static_cast<size_t>(k)].decoder) continue;
        if (seen == s.source_index) {
            slot_of_source = k;
            break;
        }
        ++seen;
    }
    if (slot_of_source < 0) {
        if (error) *error = "the plan refers to a source that is no longer loaded";
        return nullptr;
    }
    std::string err;
    const Frame* f = preview_cache_->get(slot_of_source, s.frame, &err);
    if (f == nullptr && error) *error = err;
    return f;
}

void AppState::set_preview_slot(int64_t i) {
    if (!plan_.ok) return;
    const int64_t n = static_cast<int64_t>(plan_.plan.slots.size());
    preview_slot_ = i < 0 ? 0 : (i >= n ? n - 1 : i);
}

RenderProgress AppState::progress() const {
    std::lock_guard<std::mutex> lk(render_mutex_);
    return render_;
}

void AppState::cancel_render() { render_cancel_ = true; }

void AppState::poll_render() {
    bool join = false;
    {
        std::lock_guard<std::mutex> lk(render_mutex_);
        join = render_.finished && !render_.running;
    }
    if (join && render_thread_.joinable()) render_thread_.join();
}

bool AppState::start_render() {
    {
        std::lock_guard<std::mutex> lk(render_mutex_);
        if (render_.running) {
            render_.error = "a render is already running";
            return false;
        }
    }
    if (render_thread_.joinable()) render_thread_.join();

    if (!plan_.ok) {
        std::lock_guard<std::mutex> lk(render_mutex_);
        render_ = RenderProgress{};
        render_.error = "calculate a plan first";
        render_.finished = true;
        return false;
    }
    if (out_folder_.empty()) {
        std::lock_guard<std::mutex> lk(render_mutex_);
        render_ = RenderProgress{};
        render_.error = "choose an output folder first";
        render_.finished = true;
        return false;
    }
    adopt_source_output();

    // Snapshot everything the thread needs, so the UI stays free to change its own state.
    std::vector<std::string> paths;
    for (const auto& s : slots_)
        if (s.decoder) paths.push_back(s.path);
    const Plan plan_copy = plan_.plan;
    const OutputParams out_copy = out_;
    const std::string dir = out_folder_;

    render_cancel_ = false;
    {
        std::lock_guard<std::mutex> lk(render_mutex_);
        render_ = RenderProgress{};
        render_.running = true;
        render_.total = static_cast<int64_t>(plan_copy.slots.size());
    }

    render_thread_ = std::thread([this, paths, plan_copy, out_copy, dir] {
        auto finish = [this](bool ok, bool cancelled, const std::string& err) {
            std::lock_guard<std::mutex> lk(render_mutex_);
            render_.running = false;
            render_.finished = true;
            render_.ok = ok;
            render_.cancelled = cancelled;
            if (!ok) render_.error = err;
        };

        std::string err;
        // This thread's own decoders: a Decoder serves one consumer, and the UI is using
        // the others for its previews.
        std::vector<std::unique_ptr<Decoder>> decoders;
        for (const auto& p : paths) {
            auto d = Decoder::open(p, &err);
            if (!d) {
                finish(false, false, "cannot open " + p + ": " + err);
                return;
            }
            decoders.push_back(std::move(d));
        }

        const std::string path = next_output_path(dir, &err);
        if (path.empty()) {
            finish(false, false, err);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(render_mutex_);
            render_.path = path;
        }

        auto enc = Encoder::create(path, out_copy, &err);
        if (!enc) {
            finish(false, false, err);
            return;
        }

        FrameCache cache(256u * 1024 * 1024);
        for (size_t i = 0; i < decoders.size(); ++i)
            cache.attach(static_cast<int>(i), decoders[i].get());

        int64_t done = 0;
        for (const auto& slot : plan_copy.slots) {
            if (render_cancel_) {
                finish(false, true, "cancelled");
                return;
            }
            const Frame* f = slot.kind == SlotKind::Black
                                 ? &cache.black(out_copy.width, out_copy.height)
                                 : cache.get(slot.source_index, slot.frame, &err);
            if (f == nullptr || !enc->write(*f, &err)) {
                finish(false, false, err);
                return;
            }
            ++done;
            std::lock_guard<std::mutex> lk(render_mutex_);
            render_.done = done;
        }
        if (!enc->finish(&err)) {
            finish(false, false, err);
            return;
        }
        finish(true, false, {});
    });
    return true;
}

std::string AppState::command_line() const {
    std::string c = "vrc";
    char buf[128];

    std::snprintf(buf, sizeof buf, " --seed %llu",
                  static_cast<unsigned long long>(plan_.ok ? cut_seed_ : params_.seed));
    c += buf;
    if (reordered_) {
        std::snprintf(buf, sizeof buf, " --reorder-seed %llu",
                      static_cast<unsigned long long>(order_seed_));
        c += buf;
    }
    std::snprintf(buf, sizeof buf, " --segments %d", params_.segments_per_source);
    c += buf;
    if (params_.black_total > 0) {
        std::snprintf(buf, sizeof buf, " --black %lld --black-run %lld",
                      static_cast<long long>(params_.black_total),
                      static_cast<long long>(params_.black_run_max));
        c += buf;
    }
    if (!params_.avoid_adjacent_segments) c += " --allow-adjacent";
    c += std::string(" --codec ") + out_.codec;
    c += std::string(" --scaling ") + scaling_name(out_.scaling);
    if (out_.width > 0 && out_.height > 0) {
        std::snprintf(buf, sizeof buf, " --width %d --height %d", out_.width, out_.height);
        c += buf;
    }
    if (out_.fps > 0.0) {
        std::snprintf(buf, sizeof buf, " --fps %.6g", out_.fps);
        c += buf;
    }
    if (!out_folder_.empty()) c += " --out " + quoted(out_folder_);
    for (const auto& s : slots_)
        if (s.decoder) c += " " + quoted(s.path);
    return c;
}

}  // namespace vrc
