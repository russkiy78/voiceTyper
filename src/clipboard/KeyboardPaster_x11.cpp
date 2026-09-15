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

    bool isReadyToPaste() const override {
        if (!display_)
            return true; // Let sendPaste report the unavailable display.

        char keys[32] = {};
        XQueryKeymap(display_, keys);
        const auto down = [&keys](KeyCode code) {
            return code != 0 && (static_cast<unsigned char>(keys[code / 8]) &
                                 (1u << (code % 8))) != 0;
        };

        // In particular, Ctrl+V while Alt from Ctrl+Alt+V is still held
        // triggers the recording hotkey. Releasing a modifier synthetically
        // also corrupts the state of a physically held key. Wait instead.
        bool ready = !down(XKeysymToKeycode(display_, XK_v));
        if (auto* modifiers = XGetModifierMapping(display_)) {
            for (int i = 0; i < 8 * modifiers->max_keypermod; ++i) {
                // CapsLock/NumLock are toggles and do not alter Ctrl+V.
                const int mask = 1 << (i / modifiers->max_keypermod);
                if (mask == LockMask || mask == Mod2Mask)
                    continue;
                if (down(modifiers->modifiermap[i]))
                    ready = false;
            }
            XFreeModifiermap(modifiers);
        }
        return ready;
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
