// Drives AppState with no window, no GPU, no ffmpeg and no movie files: the Decoder and
// Encoder interfaces are implemented here by fakes. What this proves is the part of the
// interface that is logic, which is all of it except the drawing.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "app/app_state.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

// ---- the fakes ------------------------------------------------------------------------
// A fake movie: `frame_count` frames of a flat colour that encodes its own index, so a
// preview can be checked against the frame that was asked for.

struct FakeSpec {
    int64_t frames = 100;
    int width = 64;
    int height = 32;
    double fps = 25.0;
};

std::map<std::string, FakeSpec> g_fakes;
int64_t g_frames_written = 0;
int g_encoders_created = 0;
std::string g_last_encoder_path;
bool g_encoder_refuses = false;

class FakeDecoder final : public vrc::Decoder {
  public:
    explicit FakeDecoder(const std::string& path, const FakeSpec& spec) {
        src_.path = path;
        src_.frame_count = spec.frames;
        src_.width = spec.width;
        src_.height = spec.height;
        src_.fps = spec.fps;
    }
    const vrc::Source& source() const override { return src_; }
    vrc::Frame frame_at(int64_t index, std::string* error) override {
        if (index < 0 || index >= src_.frame_count) {
            if (error) *error = "FakeDecoder: out of range";
            return {};
        }
        vrc::Frame f;
        f.width = src_.width;
        f.height = src_.height;
        f.stride = src_.width * 4;
        f.pixels.assign(static_cast<size_t>(f.stride) * f.height, 0);
        // Red carries the low byte of the index, alpha is opaque.
        for (size_t i = 0; i < f.pixels.size(); i += 4) {
            f.pixels[i] = static_cast<uint8_t>(index & 0xff);
            f.pixels[i + 3] = 255;
        }
        return f;
    }
    void prefetch(int64_t) override {}

  private:
    vrc::Source src_;
};

class FakeEncoder final : public vrc::Encoder {
  public:
    bool write(const vrc::Frame& f, std::string* error) override {
        if (!f.valid()) {
            if (error) *error = "FakeEncoder: invalid frame";
            return false;
        }
        ++g_frames_written;
        return true;
    }
    bool finish(std::string*) override { return true; }
};

}  // namespace

namespace vrc {

std::unique_ptr<Decoder> Decoder::open(const std::string& path, std::string* error) {
    const auto it = g_fakes.find(path);
    if (it == g_fakes.end()) {
        if (error) *error = "no such fake movie: " + path;
        return nullptr;
    }
    return std::make_unique<FakeDecoder>(path, it->second);
}

std::unique_ptr<Encoder> Encoder::create(const std::string& path, const OutputParams& out,
                                         std::string* error) {
    ++g_encoders_created;
    g_last_encoder_path = path;
    if (g_encoder_refuses) {
        if (error) *error = "FakeEncoder refuses";
        return nullptr;
    }
    if (out.width <= 0 || out.height <= 0 || !(out.fps > 0.0)) {
        if (error) *error = "FakeEncoder: output size or rate not set";
        return nullptr;
    }
    return std::make_unique<FakeEncoder>();
}

std::string next_output_path(const std::string& dir, std::string* error) {
    if (dir.empty()) {
        if (error) *error = "no output directory";
        return {};
    }
    return dir + "/vid000.mov";
}

}  // namespace vrc

namespace {

// A folder of files with the right names, so the listing and the filter can be checked.
// They are never opened: Decoder::open consults g_fakes.
std::string make_folder() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vrc_app_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* name : {"a.mov", "b.mov", "c.mp4", "d.jpg", "notes.txt", ".hidden.mov"})
        std::ofstream(dir / name) << "x";
    return dir.string();
}

void register_fakes(const std::string& dir) {
    g_fakes.clear();
    g_fakes[dir + "/a.mov"] = FakeSpec{120, 64, 32, 25.0};
    g_fakes[dir + "/b.mov"] = FakeSpec{90, 32, 32, 30.0};
    g_fakes[dir + "/c.mp4"] = FakeSpec{60, 48, 24, 24.0};
}

void wait_for_render(vrc::AppState& a) {
    for (int i = 0; i < 20000; ++i) {
        a.poll_render();
        if (!a.progress().running) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(false, "the render never finished");
}

// The folder listing honours the extension filter and hides dotfiles.
void test_folder_listing(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.folder_files().size() == 2, "the .mov filter finds two files");
    check(a.folder_files()[0] == "a.mov" && a.folder_files()[1] == "b.mov",
          "and lists them sorted, without the dotfile");

    a.set_filter(2);  // .mp4
    check(a.folder_files().size() == 1 && a.folder_files()[0] == "c.mp4", ".mp4 filter");

    a.set_filter(3);  // ALL
    check(a.folder_files().size() == 5, "ALL takes everything but the dotfile");

    vrc::AppState b;
    b.set_source_folder(dir + "/nope");
    check(b.folder_files().empty(), "a missing folder yields no listing");
}

// The steps unlock in the patch's order, and never out of it.
void test_step_gating(const std::string& dir) {
    vrc::AppState a;
    check(a.reachable_step() == vrc::Step::PickFolder, "nothing loaded: step 1");
    check(!a.step_enabled(vrc::Step::PickFiles), "no files to pick yet");
    check(!a.step_enabled(vrc::Step::Calculate), "cannot calculate with no source");
    check(!a.step_enabled(vrc::Step::Render), "cannot render with no plan");

    a.set_source_folder(dir);
    check(a.reachable_step() == vrc::Step::PickFiles, "a folder with files: step 2");

    check(a.load_source(0, dir + "/a.mov"), "loaded voice 1");
    check(a.reachable_step() == vrc::Step::SetCuts, "one source loaded: step 3");
    check(a.step_enabled(vrc::Step::Calculate), "calculate is now allowed");
    check(!a.step_enabled(vrc::Step::Combine), "combine needs a plan");

    check(a.calculate(), "calculated");
    check(a.step_enabled(vrc::Step::Combine), "combine is now allowed");
    check(a.reachable_step() == vrc::Step::SetOutput, "a plan but no output folder: step 6");
    check(!a.step_enabled(vrc::Step::Render), "render still needs an output folder");

    a.set_output_folder(dir);
    check(a.reachable_step() == vrc::Step::Render, "everything set: step 7");
    check(a.step_enabled(vrc::Step::Render), "render is allowed");
}

// Loading, unloading or adding a voice drops the plan: it described other sources.
void test_plan_invalidation(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.load_source(1, dir + "/b.mov"), "voice 2");
    check(a.calculate(), "calculated");
    check(a.has_plan(), "a plan exists");

    a.unload_source(1);
    check(!a.has_plan(), "unloading a voice drops the plan");

    check(a.load_source(1, dir + "/b.mov"), "voice 2 again");
    check(a.calculate(), "calculated again");
    check(a.add_source(), "added a third slot");
    check(!a.has_plan(), "adding a voice drops the plan");
}

// The predicted count covers every loaded voice, which the patch's readout did not.
void test_predicted_counts_every_voice(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    a.params().segments_per_source = 4;
    a.params().black_total = 10;
    a.params().black_run_max = 3;

    check(a.load_source(0, dir + "/a.mov"), "voice 1, 120 frames");
    check(a.predicted_frames() == 120 + 10, "one voice");

    check(a.load_source(1, dir + "/b.mov"), "voice 2, 90 frames");
    check(a.predicted_frames() == 120 + 90 + 10, "two voices, both counted");

    check(a.calculate(), "calculated");
    check(static_cast<int64_t>(a.plan().slots.size()) == a.predicted_frames(),
          "and the plan matches the prediction");
}

// Combine keeps the cuts, changes the order, and both seeds describe the result.
void test_combine_keeps_cuts(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.load_source(1, dir + "/b.mov"), "voice 2");
    a.params().segments_per_source = 5;
    a.params().black_total = 8;
    a.params().black_run_max = 2;
    check(a.calculate(), "calculated");

    const uint64_t cut = a.cut_seed();
    const uint64_t first_digest = a.digest();
    const size_t segments = a.plan().segments.size();
    const size_t frames = a.plan().slots.size();
    check(!a.reordered(), "a fresh calculate is not a reorder");
    check(a.order_seed() == 0, "and carries no order seed");

    check(a.recombine(), "recombined");
    check(a.reordered(), "now it is a reorder");
    check(a.order_seed() != 0, "with an order seed");
    check(a.cut_seed() == cut, "the cut seed is unchanged");
    check(a.digest() != first_digest, "the timeline changed");
    check(a.plan().segments.size() == segments, "the same number of segments");
    check(a.plan().slots.size() == frames, "the same number of frames");

    // The pair of seeds reproduces exactly what is on screen, which is what the printed
    // command line claims.
    std::vector<vrc::Source> sources = {a.slot(0).info, a.slot(1).info};
    auto rebuilt = vrc::build_plan(sources, [&] {
        vrc::RecombineParams p = a.params();
        p.seed = a.cut_seed();
        return p;
    }());
    check(rebuilt.ok, "rebuilt from the cut seed");
    auto reordered = vrc::reorder_plan(rebuilt.plan, a.order_seed(),
                                       a.params().avoid_adjacent_segments);
    check(reordered.ok, "reordered from the order seed");
    check(vrc::plan_digest(reordered.plan) == a.digest(),
          "the two seeds reproduce the plan on screen");

    const std::string cmd = a.command_line();
    check(cmd.find("--reorder-seed") != std::string::npos,
          "and the command line carries the order seed: " + cmd);
}

// Impossible parameters are refused with a reason, and leave no plan behind.
void test_refusals(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    check(!a.calculate(), "cannot calculate with no source");
    check(!a.plan_error().empty(), "and says why");

    check(a.load_source(0, dir + "/a.mov"), "voice 1, 120 frames");
    a.params().segments_per_source = 500;
    check(!a.calculate(), "more segments than frames is refused");
    check(!a.has_plan(), "and leaves no plan");
    check(!a.plan_error().empty(), "with a reason");

    check(!a.load_source(0, dir + "/missing.mov"), "a file that does not open");
    check(!a.slot(0).error.empty(), "records the reason on the slot");

    check(!a.recombine(), "cannot recombine without a plan");

    vrc::AppState b;
    b.set_source_folder(dir);
    check(b.load_source(0, dir + "/a.mov"), "voice 1");
    check(b.calculate(), "calculated");
    check(!b.start_render(), "cannot render without an output folder");
    check(!b.progress().error.empty(), "and says why");
}

// The render writes exactly the plan, through its own decoders, and reports progress.
void test_render(const std::string& dir) {
    g_frames_written = 0;
    g_encoder_refuses = false;

    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.load_source(1, dir + "/b.mov"), "voice 2");
    a.params().segments_per_source = 6;
    a.params().black_total = 15;
    a.params().black_run_max = 4;
    check(a.calculate(), "calculated");
    a.set_output_folder(dir);

    const int64_t expected = static_cast<int64_t>(a.plan().slots.size());
    check(a.start_render(), "render started");
    wait_for_render(a);

    const vrc::RenderProgress pr = a.progress();
    check(pr.finished && pr.ok, "render finished and succeeded: " + pr.error);
    check(!pr.cancelled, "not cancelled");
    check(pr.done == expected, "progress reached the end");
    check(g_frames_written == expected,
          "the encoder received exactly the plan's frame count");
    check(pr.path == dir + "/vid000.mov", "and wrote where next_output_path said");

    // The output size was empty, so it was taken from the first voice.
    check(a.output().width == 64 && a.output().height == 32, "adopted voice 1's size");
    check(a.output().fps == 25.0, "adopted voice 1's rate");
}

// A render that cannot start its encoder fails loudly rather than reporting success.
void test_render_failure(const std::string& dir) {
    g_frames_written = 0;
    g_encoder_refuses = true;

    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.calculate(), "calculated");
    a.set_output_folder(dir);
    check(a.start_render(), "render started");
    wait_for_render(a);

    const vrc::RenderProgress pr = a.progress();
    check(pr.finished && !pr.ok, "the render reports failure");
    check(!pr.error.empty(), "with a reason");
    check(g_frames_written == 0, "and wrote nothing");
    g_encoder_refuses = false;
}

// Previews come from the frame that was asked for, and a black slot is opaque black.
void test_previews(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.load_source(1, dir + "/b.mov"), "voice 2");

    const vrc::Frame* f = a.source_preview(0, 42);
    check(f != nullptr && f->valid(), "a source preview came back");
    if (f) check(f->pixels[0] == 42, "and it is the frame that was asked for");

    f = a.source_preview(0, 10000);
    check(f != nullptr, "an out of range request is clamped, not refused");
    if (f) check(f->pixels[0] == static_cast<uint8_t>((120 - 1) & 0xff), "to the last frame");

    a.params().segments_per_source = 4;
    a.params().black_total = 20;
    a.params().black_run_max = 5;
    check(a.calculate(), "calculated");
    a.output().width = 64;
    a.output().height = 32;
    a.output().fps = 25.0;

    int64_t black_slot = -1, source_slot = -1;
    for (size_t i = 0; i < a.plan().slots.size(); ++i) {
        if (a.plan().slots[i].kind == vrc::SlotKind::Black && black_slot < 0)
            black_slot = static_cast<int64_t>(i);
        if (a.plan().slots[i].kind == vrc::SlotKind::Source && source_slot < 0)
            source_slot = static_cast<int64_t>(i);
    }
    check(black_slot >= 0 && source_slot >= 0, "the plan has both kinds of slot");

    std::string err;
    const vrc::Frame* b = a.plan_preview(black_slot, &err);
    check(b != nullptr, "the black slot previews: " + err);
    if (b) {
        bool opaque_black = true;
        for (size_t i = 0; i < b->pixels.size(); ++i)
            opaque_black = opaque_black && b->pixels[i] == (i % 4 == 3 ? 255 : 0);
        check(opaque_black, "and it is opaque black, not a transparent hole");
    }

    const vrc::Frame* s = a.plan_preview(source_slot, &err);
    check(s != nullptr, "a source slot previews: " + err);
    if (s)
        check(s->pixels[0] ==
                  static_cast<uint8_t>(a.plan().slots[static_cast<size_t>(source_slot)].frame &
                                       0xff),
              "and shows the frame the plan names");

    check(a.plan_preview(-1, &err) == nullptr, "a negative slot is refused");
    check(a.plan_preview(static_cast<int64_t>(a.plan().slots.size()), &err) == nullptr,
          "and so is one past the end");

    a.set_preview_slot(1000000);
    check(a.preview_slot() == static_cast<int64_t>(a.plan().slots.size()) - 1,
          "the scrub position is clamped to the last slot");
}

// More than two voices, since the patch's two were a limit of its sign trick.
void test_many_voices(const std::string& dir) {
    vrc::AppState a;
    a.set_source_folder(dir);
    a.set_filter(3);  // ALL
    check(a.add_source(), "a third slot");
    check(a.load_source(0, dir + "/a.mov"), "voice 1");
    check(a.load_source(1, dir + "/b.mov"), "voice 2");
    check(a.load_source(2, dir + "/c.mp4"), "voice 3");
    check(a.loaded_count() == 3, "three voices loaded");

    a.params().segments_per_source = 3;
    check(a.calculate(), "calculated across three voices");
    check(a.plan().segments.size() == 9, "nine segments");
    check(a.plan().slots.size() == 120 + 90 + 60, "and every frame of all three");

    while (a.source_count() < vrc::AppState::kMaxSources) check(a.add_source(), "grew");
    check(!a.add_source(), "refuses to go past kMaxSources");
    while (a.source_count() > 1) check(a.remove_source(), "shrank");
    check(!a.remove_source(), "refuses to drop the last voice");
}

}  // namespace

int main() {
    const std::string dir = make_folder();
    register_fakes(dir);

    test_folder_listing(dir);
    test_step_gating(dir);
    test_plan_invalidation(dir);
    test_predicted_counts_every_voice(dir);
    test_combine_keeps_cuts(dir);
    test_refusals(dir);
    test_render(dir);
    test_render_failure(dir);
    test_previews(dir);
    test_many_voices(dir);

    std::filesystem::remove_all(dir);

    if (g_failures == 0) {
        std::printf("all app tests passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
