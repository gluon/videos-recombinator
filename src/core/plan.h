#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace vrc {

struct PlanResult {
    bool ok = false;
    std::string error;
    Plan plan;
    uint64_t seed_used = 0;
};

// Number of output frames the params will produce, computed from the inputs alone.
// Every frame of every source appears exactly once, plus the black budget.
int64_t predicted_length(const std::vector<Source>& sources, const RecombineParams& p);

// Cut every source into segments, place the black runs, order the segments.
PlanResult build_plan(const std::vector<Source>& sources, const RecombineParams& p);

// Redraw the segment order only. Segments and their black runs are kept, which is the
// "recalculate the combination but keep the same sequences" button of the Max patch.
PlanResult reorder_plan(const Plan& plan, uint64_t seed, bool avoid_adjacent_segments);

// Expand an ordered segment list into the flat slot timeline.
std::vector<Slot> expand(const std::vector<Segment>& ordered_segments);

// True when `b` is the segment immediately following `a` in its own source, which is the
// adjacency the ordering constraint forbids.
bool is_forbidden_pair(const Segment& a, const Segment& b);

// Fingerprint of the whole timeline: every slot's kind, source and frame, in order.
// Two plans with the same digest are the same plan. This is what makes a seed round trip
// provable from outside the process: the frame count alone is seed-independent, so
// comparing counts cannot catch a seed that was mangled on its way in.
uint64_t plan_digest(const Plan& plan);

}  // namespace vrc
