// macOS paste keystroke synthesis via Quartz CGEvent (Cmd+V).
//
// TODO (macOS): the app must be granted Accessibility permission
// (System Settings > Privacy & Security > Accessibility) for synthesized
// events to be delivered. Surface a prompt/instructions in the UI.

#include "clipboard/KeyboardPaster.h"

#include <ApplicationServices/ApplicationServices.h>

namespace vt {

namespace {

constexpr CGKeyCode kKeyV = 9; // 'v' on the standard US layout

class MacKeyboardPaster final : public KeyboardPaster {
public:
    bool sendPaste() override {
        CGEventSourceRef src =
            CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

        CGEventRef down = CGEventCreateKeyboardEvent(src, kKeyV, true);
        CGEventRef up = CGEventCreateKeyboardEvent(src, kKeyV, false);
        if (!down || !up) {
            if (down) CFRelease(down);
            if (up) CFRelease(up);
            if (src) CFRelease(src);
            return false;
        }

        CGEventSetFlags(down, kCGEventFlagMaskCommand);
        CGEventSetFlags(up, kCGEventFlagMaskCommand);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);

        CFRelease(down);
        CFRelease(up);
        if (src)
            CFRelease(src);
        return true;
    }
};

} // namespace

std::unique_ptr<KeyboardPaster> KeyboardPaster::create() {
    return std::make_unique<MacKeyboardPaster>();
}

} // namespace vt
