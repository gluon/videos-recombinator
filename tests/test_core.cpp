// Core tests. No ffmpeg, no GPU, no files: this binary must build and pass anywhere,
// which is what makes it the first thing to run after any change to the planner.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/plan.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

std::vector<vrc::Source> two_sources(int64_t a, int64_t b) {
    return {{"a.mov", a, 1920, 1080, 25.0}, {"b.mov", b, 1280, 720, 30.0}};
}

// Every frame of every source appears exactly once, the last frame included (D3),
// and no segment is empty (D7).
void test_coverage() {
    const auto sources = two_sources(381, 378);
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        vrc::RecombineParams p;
        p.seed = seed;
        p.segments_per_source = 7;
        p.black_total = 23;
        p.black_run_max = 4;
        const auto r = vrc::build_plan(sources, p);
        if (!r.ok) {
            check(false, "build_plan failed: " + r.error);
            return;
        }

        std::map<int, std::set<int64_t>> seen;
        int64_t black = 0;
        for (const auto& s : r.plan.slots) {
            if (s.kind == vrc::SlotKind::Black) {
                ++black;
                continue;
            }
            const bool fresh = seen[s.source_index].insert(s.frame).second;
            if (!fresh) {
                check(false, "frame emitted twice, seed " + std::to_string(seed));
                return;
            }
        }
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& got = seen[static_cast<int>(i)];
            if (static_cast<int64_t>(got.size()) != sources[i].frame_count) {
                check(false, "source " + std::to_string(i) + " coverage, seed " +
                                 std::to_string(seed));
                return;
            }
            if (*got.begin() != 0 || *got.rbegin() != sources[i].frame_count - 1) {
                check(false, "source " + std::to_string(i) +
                                 " does not span [0, N-1], seed " + std::to_string(seed));
                return;
            }
        }
        if (black != p.black_total) {
            check(false, "black count, seed " + std::to_string(seed));
            return;
        }
        for (const auto& seg : r.plan.segments) {
            if (seg.length() < 1) {
                check(false, "empty segment, seed " + std::to_string(seed));
                return;
            }
        }
    }
    check(true, "coverage");
}

// Divergence D4: the predicted length counts every source, not just the first one,
// and it matches the plan exactly.
void test_predicted_length() {
    const auto sources = two_sources(381, 378);
    vrc::RecombineParams p;
    p.seed = 42;
    p.segments_per_source = 5;
    p.black_total = 40;
    p.black_run_max = 6;
    const auto r = vrc::build_plan(sources, p);
    check(r.ok, "build_plan ok");
    check(vrc::predicted_length(sources, p) == 381 + 378 + 40, "predicted formula");
    check(static_cast<int64_t>(r.plan.slots.size()) == vrc::predicted_length(sources, p),
          "plan size matches prediction");
}

// Divergence D1: a black run is contiguous in the output. Tested on expand() directly so
// that the assertion cannot be blurred by two runs landing next to each other.
void test_black_run_contiguity() {
    vrc::Segment seg;
    seg.source_index = 0;
    seg.segment_index = 0;
    seg.first_frame = 100;
    seg.last_frame = 109;  // 10 frames
    seg.black_runs.push_back({3, 4});
    seg.black_runs.push_back({10, 2});  // at the very end

    const auto slots = vrc::expand({seg});
    check(slots.size() == 16, "expanded size");

    std::string shape;
    for (const auto& s : slots) shape += (s.kind == vrc::SlotKind::Black ? 'B' : '.');
    check(shape == "...BBBB.......BB", "black runs contiguous and placed, got " + shape);

    check(slots[0].frame == 100, "first source frame");
    check(slots[7].frame == 103, "frame after the run resumes where it left off");
    check(slots[13].frame == 109, "last source frame emitted");
}

// Divergence D2: no two temporally adjacent segments of the same source play back to
// back, and the guarantee holds in both directions, over many draws.
void test_ordering_constraint() {
    const auto sources = two_sources(200, 200);
    for (uint64_t seed = 1; seed <= 500; ++seed) {
        vrc::RecombineParams p;
        p.seed = seed;
        p.segments_per_source = 6;
        p.avoid_adjacent_segments = true;
        const auto r = vrc::build_plan(sources, p);
        if (!r.ok) {
            check(false, "build_plan failed: " + r.error);
            return;
        }
        for (size_t i = 0; i + 1 < r.plan.segments.size(); ++i) {
            if (vrc::is_forbidden_pair(r.plan.segments[i], r.plan.segments[i + 1])) {
                check(false, "forbidden adjacency at " + std::to_string(i) + ", seed " +
                                 std::to_string(seed));
                return;
            }
        }
    }
    check(true, "ordering constraint");
}

// Same seed, same plan, byte for byte. This is what the Max patch could not do.
void test_determinism() {
    const auto sources = two_sources(381, 378);
    vrc::RecombineParams p;
    p.seed = 123456789;
    p.segments_per_source = 9;
    p.black_total = 31;
    p.black_run_max = 5;

    const auto a = vrc::build_plan(sources, p);
    const auto b = vrc::build_plan(sources, p);
    check(a.ok && b.ok, "both plans built");
    check(a.plan.slots.size() == b.plan.slots.size(), "same size");
    bool identical = a.plan.slots.size() == b.plan.slots.size();
    for (size_t i = 0; identical && i < a.plan.slots.size(); ++i) {
        identical = a.plan.slots[i].kind == b.plan.slots[i].kind &&
                    a.plan.slots[i].source_index == b.plan.slots[i].source_index &&
                    a.plan.slots[i].frame == b.plan.slots[i].frame;
    }
    check(identical, "same seed yields the same timeline");

    vrc::RecombineParams q = p;
    q.seed = p.seed + 1;
    const auto c = vrc::build_plan(sources, q);
    check(c.ok, "third plan built");
    bool differs = false;
    for (size_t i = 0; i < a.plan.slots.size() && !differs; ++i) {
        differs = a.plan.slots[i].frame != c.plan.slots[i].frame ||
                  a.plan.slots[i].source_index != c.plan.slots[i].source_index;
    }
    check(differs, "a different seed yields a different timeline");
}

// Reordering keeps the segments and their black runs, and only changes the order.
void test_reorder_keeps_segments() {
    const auto sources = two_sources(300, 300);
    vrc::RecombineParams p;
    p.seed = 7;
    p.segments_per_source = 6;
    p.black_total = 12;
    p.black_run_max = 3;
    const auto a = vrc::build_plan(sources, p);
    check(a.ok, "built");

    const auto b = vrc::reorder_plan(a.plan, 999, true);
    check(b.ok, "reordered");
    check(a.plan.segments.size() == b.plan.segments.size(), "same segment count");
    check(a.plan.slots.size() == b.plan.slots.size(), "same total length");

    auto key = [](const vrc::Segment& s) {
        return std::to_string(s.source_index) + ":" + std::to_string(s.first_frame) + ":" +
               std::to_string(s.last_frame) + ":" + std::to_string(s.black_runs.size());
    };
    std::multiset<std::string> ka, kb;
    for (const auto& s : a.plan.segments) ka.insert(key(s));
    for (const auto& s : b.plan.segments) kb.insert(key(s));
    check(ka == kb, "the same segments, reordered");
}

// Scope errors are reported, not silently clamped.
void test_errors() {
    const auto sources = two_sources(10, 10);
    vrc::RecombineParams p;
    p.seed = 1;
    p.segments_per_source = 11;
    const auto r = vrc::build_plan(sources, p);
    check(!r.ok, "too many segments is an error");

    vrc::RecombineParams q;
    q.seed = 1;
    q.segments_per_source = 3;
    check(!vrc::build_plan({}, q).ok, "no sources is an error");
}

// Three sources, because the port is not limited to two voices.
void test_three_sources() {
    std::vector<vrc::Source> sources = {{"a.mov", 120, 1920, 1080, 25.0},
                                        {"b.mov", 90, 1920, 1080, 25.0},
                                        {"c.mov", 60, 1920, 1080, 25.0}};
    vrc::RecombineParams p;
    p.seed = 2024;
    p.segments_per_source = 4;
    p.black_total = 10;
    p.black_run_max = 3;
    const auto r = vrc::build_plan(sources, p);
    check(r.ok, "three sources plan built");
    check(static_cast<int64_t>(r.plan.slots.size()) == 120 + 90 + 60 + 10,
          "three sources length");
    check(r.plan.segments.size() == 12, "three sources segment count");
}

}  // namespace

int main() {
    test_coverage();
    test_predicted_length();
    test_black_run_contiguity();
    test_ordering_constraint();
    test_determinism();
    test_reorder_keeps_segments();
    test_errors();
    test_three_sources();

    if (g_failures == 0) {
        std::printf("all core tests passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
}
