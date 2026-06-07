#pragma once

#include <memory>

namespace vt {

// Synthesizes the platform paste shortcut into the currently focused window
// (Ctrl+V on Windows/Linux, Cmd+V on macOS). Platform implementations live in
// KeyboardPaster_{x11,win,mac}.cpp; exactly one is compiled per platform.
class KeyboardPaster {
public:
    virtual ~KeyboardPaster() = default;

    // Sends the paste key combo. Returns false if synthesis failed/unsupported.
    virtual bool sendPaste() = 0;

    static std::unique_ptr<KeyboardPaster> create();
};

} // namespace vt
