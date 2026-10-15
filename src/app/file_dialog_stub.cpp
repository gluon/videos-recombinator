#include "app/file_dialog.h"

// No native dialog on this platform. The UI falls back to a text field, and nothing
// silently returns a plausible path.

namespace vrc {

bool has_native_dialog() { return false; }

std::string pick_folder(const std::string& title, std::string* error) {
    (void)title;
    if (error) *error = "no native folder dialog in this build, type the path instead";
    return {};
}

}  // namespace vrc
