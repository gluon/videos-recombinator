#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "io/decoder.h"

namespace vrc {

// LRU over decoded frames, shared by every source, bounded by a byte budget rather than
// a frame count, because sources can differ in resolution.
class FrameCache {
  public:
    explicit FrameCache(size_t budget_bytes);
    ~FrameCache();

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    // Registers a decoder under `source_index`. The cache does not own the file.
    void attach(int source_index, Decoder* decoder);

    // Decoded frame for (source_index, frame), from the cache or through the decoder.
    const Frame* get(int source_index, int64_t frame, std::string* error);

    // A single zeroed frame of the output size, reused for every black slot.
    const Frame& black(int width, int height);

    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        size_t bytes = 0;
    };
    Stats stats() const;

  private:
    struct Impl;
    Impl* impl_;
};

}  // namespace vrc
