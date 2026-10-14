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
    //
    // The returned pointer stays valid until the next call to get() on this cache, and no
    // longer: a later call may evict the entry. The frame it points at is never evicted by
    // the call that returned it, even when one frame alone exceeds the budget. Callers
    // that need to keep a frame must copy it.
    const Frame* get(int source_index, int64_t frame, std::string* error);

    // One opaque black frame of the output size, reused for every black slot. Opaque, not
    // zeroed RGBA: a zero alpha would make black slots transparent in a format that
    // carries alpha. The returned reference stays valid until the next call with a
    // different size.
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
