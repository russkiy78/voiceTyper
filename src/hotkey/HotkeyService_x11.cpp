// X11 global hotkey via XGrabKey on the root window.
//
// The grab is issued on Qt's own X11 connection so the resulting KeyPress is
// delivered into Qt's event loop, where a QAbstractNativeEventFilter inspects
// the xcb event. We grab with the four Lock/NumLock mask combinations so the
// hotkey works regardless of CapsLock/NumLock state.

#include "hotkey/HotkeyService.h"

#include "core/Logging.h"
#include "hotkey/HotkeyParsing.h"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>

#include <xcb/xcb.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

namespace vt {

namespace {

KeySym qtKeyToKeysym(int key) {
    // Printable ASCII: Qt::Key values match the corresponding X keysyms.
    if (key >= 0x20 && key <= 0x7e)
        return static_cast<KeySym>(key);

    switch (key) {
    case Qt::Key_Escape: return XK_Escape;
    case Qt::Key_Tab: return XK_Tab;
    case Qt::Key_Backtab: return XK_ISO_Left_Tab;
    case Qt::Key_Backspace: return XK_BackSpace;
    case Qt::Key_Return: return XK_Return;
    case Qt::Key_Enter: return XK_KP_Enter;
    case Qt::Key_Insert: return XK_Insert;
    case Qt::Key_Delete: return XK_Delete;
    case Qt::Key_Home: return XK_Home;
    case Qt::Key_End: return XK_End;
    case Qt::Key_Left: return XK_Left;
    case Qt::Key_Up: return XK_Up;
    case Qt::Key_Right: return XK_Right;
    case Qt::Key_Down: return XK_Down;
    case Qt::Key_PageUp: return XK_Page_Up;
    case Qt::Key_PageDown: return XK_Page_Down;
    case Qt::Key_Space: return XK_space;
    case Qt::Key_F1: return XK_F1;
    case Qt::Key_F2: return XK_F2;
    case Qt::Key_F3: return XK_F3;
    case Qt::Key_F4: return XK_F4;
    case Qt::Key_F5: return XK_F5;
    case Qt::Key_F6: return XK_F6;
    case Qt::Key_F7: return XK_F7;
    case Qt::Key_F8: return XK_F8;
    case Qt::Key_F9: return XK_F9;
    case Qt::Key_F10: return XK_F10;
    case Qt::Key_F11: return XK_F11;
    case Qt::Key_F12: return XK_F12;
    default: return NoSymbol;
    }
}

unsigned qtModsToX11(Qt::KeyboardModifiers mods) {
    unsigned mask = 0;
    if (mods & Qt::ShiftModifier) mask |= ShiftMask;
    if (mods & Qt::ControlModifier) mask |= ControlMask;
    if (mods & Qt::AltModifier) mask |= Mod1Mask;
    if (mods & Qt::MetaModifier) mask |= Mod4Mask;
    return mask;
}

// The "ignored" modifier masks we OR into the grab so Lock/NumLock don't matter.
constexpr unsigned kIgnored[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

// X error trap used while (un)grabbing to detect conflicts (BadAccess).
bool g_xError = false;
int trapHandler(Display*, XErrorEvent*) {
    g_xError = true;
    return 0;
}

class X11HotkeyService final : public HotkeyService,
                               public QAbstractNativeEventFilter {
public:
    explicit X11HotkeyService(QObject* parent) : HotkeyService(parent) {
        if (auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>())
            display_ = x11->display();
        if (display_) {
            // Without this, holding the hotkey past the key-repeat delay makes
            // the server interleave synthetic KeyRelease/KeyPress pairs for
            // every repeat tick, so there is no way to tell "still held" from
            // "released and pressed again" by event type alone. Detectable
            // autorepeat suppresses the synthetic releases: a genuine
            // KeyRelease now only arrives when the key physically comes up,
            // which nativeEventFilter below relies on via keyDown_.
            XkbSetDetectableAutoRepeat(display_, True, nullptr);
            qApp->installNativeEventFilter(this);
        } else {
            qCWarning(vtInput) << "X11 display unavailable; global hotkey disabled";
        }
    }

    ~X11HotkeyService() override {
        unregisterHotkey();
        if (display_)
            qApp->removeNativeEventFilter(this);
    }

    bool setHotkey(const QString& sequence) override {
        if (!display_) {
            emit registrationFailed(tr("X11 display unavailable."));
            return false;
        }

        const ParsedHotkey hk = parseHotkey(sequence);
        if (!hk.valid) {
            emit registrationFailed(tr("Could not parse hotkey '%1'.").arg(sequence));
            return false;
        }

        const KeySym keysym = qtKeyToKeysym(hk.key);
        const KeyCode keycode =
            keysym == NoSymbol ? 0 : XKeysymToKeycode(display_, keysym);
        if (keycode == 0) {
            emit registrationFailed(tr("Unsupported key in '%1'.").arg(sequence));
            return false;
        }

        unregisterHotkey();

        keycode_ = keycode;
        modMask_ = qtModsToX11(hk.modifiers);

        Window root = DefaultRootWindow(display_);
        g_xError = false;
        XErrorHandler prev = XSetErrorHandler(trapHandler);
        for (unsigned ignore : kIgnored) {
            XGrabKey(display_, keycode_, modMask_ | ignore, root, False,
                     GrabModeAsync, GrabModeAsync);
        }
        XSync(display_, False);
        XSetErrorHandler(prev);

        if (g_xError) {
            unregisterHotkey();
            emit registrationFailed(
                tr("Hotkey '%1' is already in use by another application.")
                    .arg(sequence));
            return false;
        }

        registered_ = true;
        keyDown_ = false;
        qCInfo(vtInput) << "Registered global hotkey" << sequence;
        return true;
    }

    void unregisterHotkey() override {
        if (!display_ || !registered_)
            return;
        Window root = DefaultRootWindow(display_);
        XErrorHandler prev = XSetErrorHandler(trapHandler);
        for (unsigned ignore : kIgnored)
            XUngrabKey(display_, keycode_, modMask_ | ignore, root);
        XSync(display_, False);
        XSetErrorHandler(prev);
        registered_ = false;
        keyDown_ = false;
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr*) override {
        if (!registered_ || eventType != "xcb_generic_event_t")
            return false;

        auto* ev = static_cast<xcb_generic_event_t*>(message);
        const uint8_t type = ev->response_type & ~0x80;
        if (type != XCB_KEY_PRESS && type != XCB_KEY_RELEASE)
            return false;

        auto* ke = reinterpret_cast<xcb_key_press_event_t*>(ev);
        if (ke->detail != keycode_)
            return false;

        if (type == XCB_KEY_RELEASE) {
            keyDown_ = false;
            return false;
        }

        // XGrabKey also delivers the key's own repeat presses while held; with
        // detectable autorepeat those arrive as XCB_KEY_PRESS with no
        // intervening release, so keyDown_ tells a genuine press (fire once)
        // apart from a repeat tick (ignore) — otherwise holding the hotkey a
        // little longer than the key-repeat delay toggles recording on and off
        // repeatedly instead of once.
        if (keyDown_)
            return false;

        const unsigned relevant = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;
        if ((ke->state & relevant) == modMask_) {
            keyDown_ = true;
            emit activated();
        }
        return false; // never consume; let other clients see it too
    }

private:
    Display* display_ = nullptr;
    KeyCode keycode_ = 0;
    unsigned modMask_ = 0;
    bool registered_ = false;
    bool keyDown_ = false;
};

} // namespace

HotkeyService* HotkeyService::create(QObject* parent) {
    return new X11HotkeyService(parent);
}

} // namespace vt
