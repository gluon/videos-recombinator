#include "io/encoder.h"

// NOT IMPLEMENTED. Contract is in encoder.h, approach is in docs/SPEC.md section 8.

namespace vrc {

std::unique_ptr<Encoder> Encoder::create(const std::string& path, const OutputParams& out,
                                         std::string* error) {
    (void)path;
    (void)out;
    if (error) *error = "Encoder::create is not implemented yet";
    return nullptr;
}

std::string next_output_path(const std::string& dir, std::string* error) {
    (void)dir;
    if (error) *error = "next_output_path is not implemented yet";
    return {};
}

}  // namespace vrc
