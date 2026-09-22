// X11 global hotkey via XGrabKey on the root window.
//
// The grab is issued on Qt's own X11 connection so the resulting KeyPress is
// delivered into Qt's event loop, where a QAbstractNativeEventFilter arms the
// shortcut. Activation waits until every key in the combination is released.
// We grab with the four Lock/NumLock mask combinations so the
// hotkey works regardless of CapsLock/NumLock state.

#include "hotkey/HotkeyService.h"

#include "core/Logging.h"
#include "core/XdgPortal.h"
#include "hotkey/HotkeyParsing.h"
#include "hotkey/HotkeyService_portal.h"
#include "hotkey/X11Keysym.h"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QTimer>

#include <vector>

#include <xcb/xcb.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

namespace vt {

unsigned long qtKeyToKeysym(int key) {
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

namespace {

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
            XkbSetDetectableAutoRepeat(display_, True, nullptr);
            qApp->installNativeEventFilter(this);
            releaseTimer_.setInterval(10);
            connect(&releaseTimer_, &QTimer::timeout, this, [this]() {
                char keys[32] = {};
                XQueryKeymap(display_, keys);
                for (KeyCode code : combinationKeys_) {
                    if (static_cast<unsigned char>(keys[code / 8]) &
                        (1u << (code % 8)))
                        return;
                }
                // Activate on key release, after every key in the hotkey
                // combination is released, rather than on key press.
                releaseTimer_.stop();
                emit activated();
            });
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
        combinationKeys_ = {keycode_};
        if (auto* modifiers = XGetModifierMapping(display_)) {
            for (int i = 0; i < 8 * modifiers->max_keypermod; ++i) {
                const int mask = 1 << (i / modifiers->max_keypermod);
                const KeyCode code = modifiers->modifiermap[i];
                if ((modMask_ & mask) && code != 0)
                    combinationKeys_.push_back(code);
            }
            XFreeModifiermap(modifiers);
        }

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
        qCInfo(vtInput) << "Registered global hotkey" << sequence;
        return true;
    }

    void unregisterHotkey() override {
        releaseTimer_.stop();
        if (!display_ || !registered_)
            return;
        Window root = DefaultRootWindow(display_);
        XErrorHandler prev = XSetErrorHandler(trapHandler);
        for (unsigned ignore : kIgnored)
            XUngrabKey(display_, keycode_, modMask_ | ignore, root);
        XSync(display_, False);
        XSetErrorHandler(prev);
        registered_ = false;
        combinationKeys_.clear();
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr*) override {
        if (!registered_ || eventType != "xcb_generic_event_t")
            return false;

        auto* ev = static_cast<xcb_generic_event_t*>(message);
        const uint8_t type = ev->response_type & ~0x80;
        if (type != XCB_KEY_PRESS)
            return false;

        auto* ke = reinterpret_cast<xcb_key_press_event_t*>(ev);
        if (ke->detail != keycode_)
            return false;

        // Arm once; repeats cannot activate or re-arm a held combination.
        if (releaseTimer_.isActive())
            return false;

        const unsigned relevant = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;
        if ((ke->state & relevant) == modMask_) {
            // The passive grab ends when the main key comes up, so modifier
            // releases may go to another app. Query the server until all keys
            // are up instead of depending on delivery/order of KeyRelease.
            releaseTimer_.start();
        }
        return false; // never consume; let other clients see it too
    }

private:
    Display* display_ = nullptr;
    KeyCode keycode_ = 0;
    unsigned modMask_ = 0;
    bool registered_ = false;
    QTimer releaseTimer_;
    std::vector<KeyCode> combinationKeys_;
};

} // namespace

HotkeyService* HotkeyService::create(QObject* parent) {
    // On Wayland the grab below only fires while an Xwayland window has focus;
    // let the compositor own the shortcut instead.
    if (portal::waylandInput()) {
        if (portal::hasInterface(QStringLiteral("org.freedesktop.portal.GlobalShortcuts")))
            return new PortalHotkeyService(parent);
        qCWarning(vtInput) << "Wayland session without the GlobalShortcuts portal; the"
                              " X11 hotkey only works while an Xwayland window is focused";
    }
    return new X11HotkeyService(parent);
}

} // namespace vt
