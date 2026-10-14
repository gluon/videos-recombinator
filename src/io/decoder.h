#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace vrc {

// One decoded frame, RGBA8, tightly packed, `stride` bytes per row.
struct Frame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Random access reader over one movie. Opens the file once, builds a packet index in a
// prepass, and decodes on demand by seeking to the keyframe at or before the target.
// Nothing is buffered in full: that is the whole point of the port.
class Decoder {
  public:
    static std::unique_ptr<Decoder> open(const std::string& path, std::string* error);
    virtual ~Decoder() = default;

    // Probed in the prepass, not read from container metadata.
    virtual const Source& source() const = 0;

    // Decode exactly frame `index`. Returns an invalid Frame and sets `error` on failure.
    virtual Frame frame_at(int64_t index, std::string* error) = 0;

    // Hint that `index` will be asked for soon, so a sequential walk stays sequential.
    virtual void prefetch(int64_t index) = 0;
};

}  // namespace vrc
