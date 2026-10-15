#include "app/file_dialog.h"

#import <AppKit/AppKit.h>

// NSOpenPanel in folder mode, run modally from the frame callback, which is the main
// thread under sokol_app. The same gesture as `opendialog fold` in the patch.

namespace vrc {

bool has_native_dialog() { return true; }

std::string pick_folder(const std::string& title, std::string* error) {
    (void)error;
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = YES;
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        if ([panel runModal] != NSModalResponseOK) return {};
        NSURL* url = panel.URLs.firstObject;
        if (url == nil || url.path == nil) return {};
        return std::string(url.path.UTF8String);
    }
}

}  // namespace vrc
