#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrc {

// A movie file the recombinator draws frames from. frame_count, width, height and fps
// come from the decoder prepass, never from container metadata.
struct Source {
    std::string path;
    int64_t frame_count = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
};

enum class SlotKind { Source, Black };

// One output frame. The voice is source_index, a field. It is never encoded in the sign
// of `frame`, which is what the Max patch did.
struct Slot {
    SlotKind kind = SlotKind::Source;
    int source_index = 0;
    int64_t frame = 0;
};

// A contiguous run of frames of one source, with the black runs inserted into it.
// Both ends inclusive, so length() is never zero.
struct Segment {
    struct BlackRun {
        int64_t offset = 0;  // in [0, length()], black frames go before frame `offset`
        int64_t length = 0;
    };

    int source_index = 0;
    int segment_index = 0;  // position of this segment within its own source, in time
    int64_t first_frame = 0;
    int64_t last_frame = 0;
    std::vector<BlackRun> black_runs;

    int64_t length() const { return last_frame - first_frame + 1; }
};

enum class Scaling { Stretch, Fit, Fill };

struct RecombineParams {
    uint64_t seed = 0;  // 0 means draw one from system entropy and report it
    int segments_per_source = 8;
    int64_t black_total = 0;
    int64_t black_run_max = 1;
    bool avoid_adjacent_segments = true;
};

struct OutputParams {
    int width = 0;   // 0 means take it from source 0
    int height = 0;
    double fps = 0.0;  // 0 means take it from source 0
    Scaling scaling = Scaling::Fit;
    std::string codec = "prores422";
};

// The complete output timeline, computed before a single frame is decoded.
struct Plan {
    std::vector<Segment> segments;  // in playback order
    std::vector<Slot> slots;
};

}  // namespace vrc
