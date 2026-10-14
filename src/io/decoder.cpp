#include "io/decoder.h"

// NOT IMPLEMENTED. Contract is in decoder.h, approach is in docs/SPEC.md section 7.
// This stub exists so the target list and the CMake wiring are real; it fails loudly
// rather than returning plausible empty frames.

namespace vrc {

std::unique_ptr<Decoder> Decoder::open(const std::string& path, std::string* error) {
    (void)path;
    if (error) *error = "Decoder::open is not implemented yet";
    return nullptr;
}

}  // namespace vrc
