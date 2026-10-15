#pragma once

#include <string>

namespace vrc {

// Asks the user for a folder, the gesture the patch's `opendialog fold` performed.
// Returns an empty string when the user cancelled or no native dialog exists on this
// platform, with the reason in `error` only in the second case.
std::string pick_folder(const std::string& title, std::string* error);

// True when this build can actually open a dialog. The UI uses it to offer a text field
// instead of a button that would do nothing.
bool has_native_dialog();

}  // namespace vrc
