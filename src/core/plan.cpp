#include "plan.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_set>

namespace vrc {
namespace {

constexpr int kOrderAttempts = 64;

uint64_t entropy_seed() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
}

int64_t uniform(std::mt19937_64& rng, int64_t lo, int64_t hi) {  // inclusive
    return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
}

// Floyd's combination sampling: k distinct values from [lo, hi], without building the
// whole pool. This is the job bigUrn.js did with a shuffled array.
std::vector<int64_t> sample_distinct(std::mt19937_64& rng, int64_t lo, int64_t hi,
                                     int64_t k) {
    std::unordered_set<int64_t> chosen;
    std::vector<int64_t> out;
    out.reserve(static_cast<size_t>(k));
    const int64_t n = hi - lo + 1;
    for (int64_t j = n - k; j < n; ++j) {
        const int64_t t = uniform(rng, lo, lo + j);
        const int64_t v = chosen.insert(t).second ? t : lo + j;
        if (v != t) chosen.insert(v);
        out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Segment> cut_source(std::mt19937_64& rng, int source_index, int64_t frames,
                                int segments) {
    // segments - 1 interior cut points, distinct, drawn from [1, frames - 1].
    // Boundaries are 0, cuts..., frames. Every segment is non-empty and the union is
    // exactly [0, frames - 1], last frame included. This is divergence D3 and D7 fixed.
    std::vector<int64_t> boundaries;
    boundaries.reserve(static_cast<size_t>(segments) + 1);
    boundaries.push_back(0);
    if (segments > 1) {
        const auto cuts = sample_distinct(rng, 1, frames - 1, segments - 1);
        boundaries.insert(boundaries.end(), cuts.begin(), cuts.end());
    }
    boundaries.push_back(frames);

    std::vector<Segment> out;
    out.reserve(static_cast<size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        Segment s;
        s.source_index = source_index;
        s.segment_index = i;
        s.first_frame = boundaries[static_cast<size_t>(i)];
        s.last_frame = boundaries[static_cast<size_t>(i) + 1] - 1;
        out.push_back(s);
    }
    return out;
}

// Black frames go in as contiguous runs, which is divergence D1 fixed: the original
// spliced every frame of a batch at its own independent index, scattering it.
void place_black_runs(std::mt19937_64& rng, std::vector<Segment>& segments,
                      int64_t total, int64_t run_max) {
    if (total <= 0 || segments.empty()) return;
    const int64_t cap = std::max<int64_t>(1, run_max);
    int64_t remaining = total;
    while (remaining > 0) {
        const int64_t run = uniform(rng, 1, std::min(cap, remaining));
        Segment& s = segments[static_cast<size_t>(
            uniform(rng, 0, static_cast<int64_t>(segments.size()) - 1))];
        s.black_runs.push_back({uniform(rng, 0, s.length()), run});
        remaining -= run;
    }
}

bool order_is_valid(const std::vector<Segment>& o) {
    for (size_t i = 0; i + 1 < o.size(); ++i) {
        if (is_forbidden_pair(o[i], o[i + 1])) return false;
    }
    return true;
}

bool shuffle_until_valid(std::mt19937_64& rng, std::vector<Segment>& order,
                         bool avoid_adjacent) {
    for (int attempt = 0; attempt < kOrderAttempts; ++attempt) {
        std::shuffle(order.begin(), order.end(), rng);
        if (!avoid_adjacent || order_is_valid(order)) return true;
    }
    return false;
}

}  // namespace

bool is_forbidden_pair(const Segment& a, const Segment& b) {
    return a.source_index == b.source_index && b.segment_index == a.segment_index + 1;
}

uint64_t plan_digest(const Plan& plan) {
    // FNV-1a over the bytes that define the timeline. Not a cryptographic hash: it only
    // has to change when the timeline changes.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (b * 8)) & 0xff;
            h *= 1099511628211ULL;
        }
    };
    for (const auto& s : plan.slots) {
        mix(s.kind == SlotKind::Black ? 1u : 0u);
        mix(static_cast<uint64_t>(s.source_index));
        mix(static_cast<uint64_t>(s.frame));
    }
    return h;
}

int64_t predicted_length(const std::vector<Source>& sources, const RecombineParams& p) {
    int64_t total = 0;
    for (const auto& s : sources) total += s.frame_count;
    return total + std::max<int64_t>(0, p.black_total);
}

std::vector<Slot> expand(const std::vector<Segment>& ordered_segments) {
    std::vector<Slot> slots;
    for (const auto& seg : ordered_segments) {
        auto runs = seg.black_runs;
        std::stable_sort(runs.begin(), runs.end(),
                         [](const Segment::BlackRun& a, const Segment::BlackRun& b) {
                             return a.offset < b.offset;
                         });
        size_t r = 0;
        const int64_t len = seg.length();
        for (int64_t pos = 0; pos <= len; ++pos) {
            while (r < runs.size() && runs[r].offset == pos) {
                for (int64_t k = 0; k < runs[r].length; ++k) {
                    Slot black;
                    black.kind = SlotKind::Black;
                    slots.push_back(black);
                }
                ++r;
            }
            if (pos < len) {
                Slot s;
                s.kind = SlotKind::Source;
                s.source_index = seg.source_index;
                s.frame = seg.first_frame + pos;
                slots.push_back(s);
            }
        }
    }
    return slots;
}

PlanResult build_plan(const std::vector<Source>& sources, const RecombineParams& p) {
    PlanResult r;
    if (sources.empty()) {
        r.error = "no sources";
        return r;
    }
    if (p.segments_per_source < 1) {
        r.error = "segments_per_source must be at least 1";
        return r;
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].frame_count < 1) {
            r.error = "source " + std::to_string(i) + " has no frames";
            return r;
        }
        if (p.segments_per_source > sources[i].frame_count) {
            r.error = "source " + std::to_string(i) + " has only " +
                      std::to_string(sources[i].frame_count) + " frames, cannot cut it " +
                      "into " + std::to_string(p.segments_per_source) + " segments";
            return r;
        }
    }
    if (p.black_total < 0) {
        r.error = "black_total must not be negative";
        return r;
    }

    r.seed_used = p.seed != 0 ? p.seed : entropy_seed();
    std::mt19937_64 rng(r.seed_used);

    std::vector<Segment> segments;
    for (size_t i = 0; i < sources.size(); ++i) {
        auto cut = cut_source(rng, static_cast<int>(i), sources[i].frame_count,
                              p.segments_per_source);
        segments.insert(segments.end(), cut.begin(), cut.end());
    }

    place_black_runs(rng, segments, p.black_total, p.black_run_max);

    if (!shuffle_until_valid(rng, segments, p.avoid_adjacent_segments)) {
        r.error = "no segment order found in " + std::to_string(kOrderAttempts) +
                  " attempts that avoids temporally adjacent segments of the same source";
        return r;
    }

    r.plan.segments = std::move(segments);
    r.plan.slots = expand(r.plan.segments);
    r.ok = true;
    return r;
}

PlanResult reorder_plan(const Plan& plan, uint64_t seed, bool avoid_adjacent_segments) {
    PlanResult r;
    if (plan.segments.empty()) {
        r.error = "plan has no segments";
        return r;
    }
    r.seed_used = seed != 0 ? seed : entropy_seed();
    std::mt19937_64 rng(r.seed_used);

    std::vector<Segment> order = plan.segments;
    if (!shuffle_until_valid(rng, order, avoid_adjacent_segments)) {
        r.error = "no segment order found in " + std::to_string(kOrderAttempts) +
                  " attempts that avoids temporally adjacent segments of the same source";
        return r;
    }
    r.plan.segments = std::move(order);
    r.plan.slots = expand(r.plan.segments);
    r.ok = true;
    return r;
}

}  // namespace vrc
