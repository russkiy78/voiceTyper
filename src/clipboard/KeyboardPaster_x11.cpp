// X11 paste keystroke synthesis via the XTEST extension.
//
// NOTE (Wayland): XTEST only reaches X11 / XWayland clients. Synthesizing input
// into native Wayland windows is intentionally restricted by the compositor.
// TODO: add a Wayland path (e.g. wlroots virtual-keyboard protocol, or
// ydotool/uinput) for native Wayland targets.

#include "clipboard/KeyboardPaster.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace vt {

namespace {

class X11KeyboardPaster final : public KeyboardPaster {
public:
    X11KeyboardPaster() { display_ = XOpenDisplay(nullptr); }
    ~X11KeyboardPaster() override {
        if (display_)
            XCloseDisplay(display_);
    }

    bool sendPaste() override {
        if (!display_)
            return false;

        const KeyCode ctrl = XKeysymToKeycode(display_, XK_Control_L);
        const KeyCode v = XKeysymToKeycode(display_, XK_v);
        if (ctrl == 0 || v == 0)
            return false;

        XTestFakeKeyEvent(display_, ctrl, True, 0);
        XTestFakeKeyEvent(display_, v, True, 0);
        XTestFakeKeyEvent(display_, v, False, 0);
        XTestFakeKeyEvent(display_, ctrl, False, 0);
        XFlush(display_);
        return true;
    }

private:
    Display* display_ = nullptr;
};

} // namespace

std::unique_ptr<KeyboardPaster> KeyboardPaster::create() {
    return std::make_unique<X11KeyboardPaster>();
}

} // namespace vt
