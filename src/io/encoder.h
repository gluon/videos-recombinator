#pragma once

#include <memory>
#include <string>

#include "core/types.h"
#include "io/decoder.h"

namespace vrc {

// Writes frames to one movie file. Not real time, not clocked: the renderer pushes
// frames as fast as it can produce them.
class Encoder {
  public:
    // `codec` is one of prores422, prores4444, h264, mjpeg.
    static std::unique_ptr<Encoder> create(const std::string& path,
                                           const OutputParams& out, std::string* error);
    virtual ~Encoder() = default;

    // Scales and pads `frame` to the output size under OutputParams::scaling, then
    // writes it. A frame already at the output size is written without resampling.
    virtual bool write(const Frame& frame, std::string* error) = 0;

    // Flushes and closes. Must be called; the destructor does not report failures.
    virtual bool finish(std::string* error) = 0;
};

// Resolves <dir>/vid%03d.mov to the first index not already on disk.
std::string next_output_path(const std::string& dir, std::string* error);

}  // namespace vrc
