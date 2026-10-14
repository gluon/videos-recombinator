// vrc: plan and render from the command line. The planner half works today; the render
// half calls into src/io, which is still stubbed, and says so instead of pretending.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/plan.h"
#include "io/decoder.h"
#include "io/encoder.h"
#include "io/frame_cache.h"

namespace {

void usage() {
    std::printf(
        "usage: vrc [options] <movie> <movie> [<movie>...]\n"
        "\n"
        "  --segments N       segments cut per source (default 8)\n"
        "  --black N          total black frames to insert (default 0)\n"
        "  --black-run N      longest contiguous black run (default 1)\n"
        "  --seed N           0 draws one and reports it (default 0), full 64 bits\n"
        "  --allow-adjacent   do not forbid temporally adjacent segments of one source\n"
        "  --out DIR          output directory, writes DIR/vid%%03d.mov\n"
        "  --codec NAME       prores422 | prores4444 | h264 | mjpeg\n"
        "  --scaling MODE     fit (default) | stretch | fill, for sources that differ\n"
        "                     from the output size\n"
        "  --width N --height N --fps F   default to source 0\n"
        "  --cache-mb N       decoded frame cache budget (default 1024)\n"
        "  --dry-run          plan only, print the timeline summary, write nothing\n"
        "\n"
        "The `plan` line is a fingerprint of the whole timeline. Two runs that print the\n"
        "same plan render the same frames in the same order; the `frames` count alone does\n"
        "not depend on the seed, so it cannot tell two seeds apart.\n");
}

bool parse_i64(const char* s, int64_t* out) {
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return false;
    *out = static_cast<int64_t>(v);
    return true;
}

// Seeds use all 64 bits, so they are not int64_t. strtoll would silently saturate every
// seed above 2^63-1 to INT64_MAX, and entropy_seed() sets the top bit about half the time,
// so about one drawn seed in two would not replay.
bool parse_u64(const char* s, uint64_t* out) {
    char* end = nullptr;
    errno = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') return false;  // strtoull wraps a negative, which is not a typo to keep
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    vrc::RecombineParams params;
    vrc::OutputParams out;
    std::vector<std::string> inputs;
    std::string out_dir;
    int64_t cache_mb = 1024;
    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](int64_t* dst) {
            if (i + 1 >= argc || !parse_i64(argv[++i], dst)) {
                std::fprintf(stderr, "bad value for %s\n", a.c_str());
                std::exit(EXIT_FAILURE);
            }
        };
        int64_t v = 0;
        if (a == "-h" || a == "--help") {
            usage();
            return EXIT_SUCCESS;
        } else if (a == "--segments") {
            need(&v);
            params.segments_per_source = static_cast<int>(v);
        } else if (a == "--black") {
            need(&params.black_total);
        } else if (a == "--black-run") {
            need(&params.black_run_max);
        } else if (a == "--seed") {
            if (i + 1 >= argc || !parse_u64(argv[++i], &params.seed)) {
                std::fprintf(stderr,
                             "bad value for --seed: expected an integer in [0, 2^64-1]\n");
                return EXIT_FAILURE;
            }
        } else if (a == "--scaling") {
            if (i + 1 >= argc) { usage(); return EXIT_FAILURE; }
            const std::string mode = argv[++i];
            if (mode == "fit") {
                out.scaling = vrc::Scaling::Fit;
            } else if (mode == "stretch") {
                out.scaling = vrc::Scaling::Stretch;
            } else if (mode == "fill") {
                out.scaling = vrc::Scaling::Fill;
            } else {
                std::fprintf(stderr,
                             "bad value for --scaling: '%s', expected fit, stretch or fill\n",
                             mode.c_str());
                return EXIT_FAILURE;
            }
        } else if (a == "--allow-adjacent") {
            params.avoid_adjacent_segments = false;
        } else if (a == "--out") {
            if (i + 1 >= argc) { usage(); return EXIT_FAILURE; }
            out_dir = argv[++i];
        } else if (a == "--codec") {
            if (i + 1 >= argc) { usage(); return EXIT_FAILURE; }
            out.codec = argv[++i];
        } else if (a == "--width") {
            need(&v);
            out.width = static_cast<int>(v);
        } else if (a == "--height") {
            need(&v);
            out.height = static_cast<int>(v);
        } else if (a == "--fps") {
            if (i + 1 >= argc) { usage(); return EXIT_FAILURE; }
            out.fps = std::strtod(argv[++i], nullptr);
        } else if (a == "--cache-mb") {
            need(&cache_mb);
        } else if (a == "--dry-run") {
            dry_run = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return EXIT_FAILURE;
        } else {
            inputs.push_back(a);
        }
    }

    if (inputs.empty()) {
        usage();
        return EXIT_FAILURE;
    }
    // A render needs somewhere to put the file. Defaulting to the working directory would
    // drop multi-gigabyte movies wherever the shell happens to stand.
    if (!dry_run && out_dir.empty()) {
        std::fprintf(stderr,
                     "no output directory: pass --out DIR to render, or --dry-run to plan "
                     "without writing anything\n");
        return EXIT_FAILURE;
    }

    // Probe every source. Dimensions, frame count and fps come from here.
    std::vector<vrc::Source> sources;
    std::vector<std::unique_ptr<vrc::Decoder>> decoders;
    for (const auto& path : inputs) {
        std::string err;
        auto d = vrc::Decoder::open(path, &err);
        if (!d) {
            std::fprintf(stderr, "cannot open %s: %s\n", path.c_str(), err.c_str());
            return EXIT_FAILURE;
        }
        sources.push_back(d->source());
        decoders.push_back(std::move(d));
    }

    const auto planned = vrc::build_plan(sources, params);
    if (!planned.ok) {
        std::fprintf(stderr, "cannot plan: %s\n", planned.error.c_str());
        return EXIT_FAILURE;
    }

    std::printf("seed      %llu\n", static_cast<unsigned long long>(planned.seed_used));
    std::printf("sources   %zu\n", sources.size());
    std::printf("segments  %zu\n", planned.plan.segments.size());
    std::printf("frames    %zu (predicted %lld)\n", planned.plan.slots.size(),
                static_cast<long long>(vrc::predicted_length(sources, params)));
    std::printf("plan      %016llx\n",
                static_cast<unsigned long long>(vrc::plan_digest(planned.plan)));

    if (dry_run) return EXIT_SUCCESS;

    if (out.width == 0) out.width = sources[0].width;
    if (out.height == 0) out.height = sources[0].height;
    if (out.fps == 0.0) out.fps = sources[0].fps;

    std::string err;
    const std::string path = vrc::next_output_path(out_dir, &err);
    if (path.empty()) {
        std::fprintf(stderr, "cannot pick an output path: %s\n", err.c_str());
        return EXIT_FAILURE;
    }
    auto encoder = vrc::Encoder::create(path, out, &err);
    if (!encoder) {
        std::fprintf(stderr, "cannot create the encoder: %s\n", err.c_str());
        return EXIT_FAILURE;
    }

    vrc::FrameCache cache(static_cast<size_t>(cache_mb) * 1024 * 1024);
    for (size_t i = 0; i < decoders.size(); ++i)
        cache.attach(static_cast<int>(i), decoders[i].get());

    for (const auto& slot : planned.plan.slots) {
        const vrc::Frame* f = slot.kind == vrc::SlotKind::Black
                                  ? &cache.black(out.width, out.height)
                                  : cache.get(slot.source_index, slot.frame, &err);
        if (f == nullptr || !encoder->write(*f, &err)) {
            std::fprintf(stderr, "render failed: %s\n", err.c_str());
            return EXIT_FAILURE;
        }
    }
    if (!encoder->finish(&err)) {
        std::fprintf(stderr, "cannot close %s: %s\n", path.c_str(), err.c_str());
        return EXIT_FAILURE;
    }

    std::printf("wrote     %s\n", path.c_str());
    return EXIT_SUCCESS;
}
