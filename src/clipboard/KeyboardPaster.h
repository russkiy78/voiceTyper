#pragma once

#include <memory>

namespace vt {

// Synthesizes the platform paste shortcut into the currently focused window
// (Ctrl+V on Windows/Linux, Cmd+V on macOS). Platform implementations live in
// KeyboardPaster_{x11,portal,win,mac}.cpp; create() returns the right one.
class KeyboardPaster {
public:
    virtual ~KeyboardPaster() = default;

    // A paste is coming up: backends that need setup first (the Wayland portal
    // session) start it here and report readiness via isReadyToPaste().
    virtual void prepare() {}

    // Wait before synthesis if held keys would change the paste shortcut.
    virtual bool isReadyToPaste() const { return true; }

    // Sends the paste key combo. Returns false if synthesis failed/unsupported.
    virtual bool sendPaste() = 0;

    static std::unique_ptr<KeyboardPaster> create();
};

} // namespace vt
