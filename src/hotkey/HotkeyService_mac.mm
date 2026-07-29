// macOS global hotkey via NSEvent global/local monitors.
//
// This used to be implemented with Carbon's RegisterEventHotKey, the
// textbook way to get a system-wide hotkey without needing Accessibility
// permission. On macOS 26 it registers without error but the callback never
// fires for a real key press — RegisterEventHotKey is a dead end on this OS
// version (matches widely-reported Tahoe regressions). NSEvent monitors are
// the modern replacement Apple's own docs point to; the tradeoff is they do
// require Accessibility (or Input Monitoring) permission, which is granted
// once in System Settings > Privacy & Security.
//
// Modifier mapping note: this app does not set AA_MacDontSwapCtrlAndMeta, so
// Qt's default macOS convention applies — the physical ⌘ key is reported as
// Qt::ControlModifier and the physical Control key as Qt::MetaModifier. The
// Settings UI captures the sequence with QKeySequenceEdit under that same
// convention, so mirroring it here (Control->Command, Meta->Control) makes a
// hotkey captured on this Mac register on the same physical keys.

#include "hotkey/HotkeyService.h"

#include "core/Logging.h"
#include "hotkey/HotkeyParsing.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h> // kVK_* virtual keycode constants only

namespace vt {

namespace {

constexpr unsigned short kInvalidVk = 0xFFFF;

NSEventModifierFlags qtModsToCocoa(Qt::KeyboardModifiers mods) {
    NSEventModifierFlags m = 0;
    if (mods & Qt::ControlModifier) m |= NSEventModifierFlagCommand; // physical Cmd
    if (mods & Qt::AltModifier) m |= NSEventModifierFlagOption;      // physical Option
    if (mods & Qt::ShiftModifier) m |= NSEventModifierFlagShift;     // physical Shift
    if (mods & Qt::MetaModifier) m |= NSEventModifierFlagControl;    // physical Control
    return m;
}

// ANSI virtual keycodes (US keyboard layout positions — these identify a
// physical key position, not a character, same as the old Carbon table).
unsigned short qtKeyToVk(int key) {
    switch (key) {
    case Qt::Key_A: return kVK_ANSI_A;
    case Qt::Key_B: return kVK_ANSI_B;
    case Qt::Key_C: return kVK_ANSI_C;
    case Qt::Key_D: return kVK_ANSI_D;
    case Qt::Key_E: return kVK_ANSI_E;
    case Qt::Key_F: return kVK_ANSI_F;
    case Qt::Key_G: return kVK_ANSI_G;
    case Qt::Key_H: return kVK_ANSI_H;
    case Qt::Key_I: return kVK_ANSI_I;
    case Qt::Key_J: return kVK_ANSI_J;
    case Qt::Key_K: return kVK_ANSI_K;
    case Qt::Key_L: return kVK_ANSI_L;
    case Qt::Key_M: return kVK_ANSI_M;
    case Qt::Key_N: return kVK_ANSI_N;
    case Qt::Key_O: return kVK_ANSI_O;
    case Qt::Key_P: return kVK_ANSI_P;
    case Qt::Key_Q: return kVK_ANSI_Q;
    case Qt::Key_R: return kVK_ANSI_R;
    case Qt::Key_S: return kVK_ANSI_S;
    case Qt::Key_T: return kVK_ANSI_T;
    case Qt::Key_U: return kVK_ANSI_U;
    case Qt::Key_V: return kVK_ANSI_V;
    case Qt::Key_W: return kVK_ANSI_W;
    case Qt::Key_X: return kVK_ANSI_X;
    case Qt::Key_Y: return kVK_ANSI_Y;
    case Qt::Key_Z: return kVK_ANSI_Z;
    case Qt::Key_0: return kVK_ANSI_0;
    case Qt::Key_1: return kVK_ANSI_1;
    case Qt::Key_2: return kVK_ANSI_2;
    case Qt::Key_3: return kVK_ANSI_3;
    case Qt::Key_4: return kVK_ANSI_4;
    case Qt::Key_5: return kVK_ANSI_5;
    case Qt::Key_6: return kVK_ANSI_6;
    case Qt::Key_7: return kVK_ANSI_7;
    case Qt::Key_8: return kVK_ANSI_8;
    case Qt::Key_9: return kVK_ANSI_9;
    case Qt::Key_Space: return kVK_Space;
    case Qt::Key_Escape: return kVK_Escape;
    case Qt::Key_Tab: return kVK_Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter: return kVK_Return;
    case Qt::Key_Backspace: return kVK_Delete;
    case Qt::Key_Delete: return kVK_ForwardDelete;
    case Qt::Key_Home: return kVK_Home;
    case Qt::Key_End: return kVK_End;
    case Qt::Key_Left: return kVK_LeftArrow;
    case Qt::Key_Up: return kVK_UpArrow;
    case Qt::Key_Right: return kVK_RightArrow;
    case Qt::Key_Down: return kVK_DownArrow;
    case Qt::Key_PageUp: return kVK_PageUp;
    case Qt::Key_PageDown: return kVK_PageDown;
    case Qt::Key_F1: return kVK_F1;
    case Qt::Key_F2: return kVK_F2;
    case Qt::Key_F3: return kVK_F3;
    case Qt::Key_F4: return kVK_F4;
    case Qt::Key_F5: return kVK_F5;
    case Qt::Key_F6: return kVK_F6;
    case Qt::Key_F7: return kVK_F7;
    case Qt::Key_F8: return kVK_F8;
    case Qt::Key_F9: return kVK_F9;
    case Qt::Key_F10: return kVK_F10;
    case Qt::Key_F11: return kVK_F11;
    case Qt::Key_F12: return kVK_F12;
    default: return kInvalidVk;
    }
}

class MacHotkeyService final : public HotkeyService {
public:
    explicit MacHotkeyService(QObject* parent) : HotkeyService(parent) {}

    ~MacHotkeyService() override { unregisterHotkey(); }

    bool setHotkey(const QString& sequence) override {
        const ParsedHotkey hk = parseHotkey(sequence);
        if (!hk.valid) {
            emit registrationFailed(tr("Could not parse hotkey '%1'.").arg(sequence));
            return false;
        }

        const unsigned short vk = qtKeyToVk(hk.key);
        if (vk == kInvalidVk) {
            emit registrationFailed(tr("Unsupported key in '%1'.").arg(sequence));
            return false;
        }

        unregisterHotkey();

        targetVk_ = vk;
        targetMods_ = qtModsToCocoa(hk.modifiers);

        const bool trusted = AXIsProcessTrusted();
        qCInfo(vtInput) << "AXIsProcessTrusted() =" << trusted << "for hotkey" << sequence
                         << "(vk=" << targetVk_ << "mods=" << targetMods_ << ")";
        if (!trusted) {
            emit registrationFailed(
                tr("Hotkey '%1' needs Accessibility (or Input Monitoring) permission — "
                   "grant voiceTyper access in System Settings > Privacy & Security, "
                   "then restart it.")
                    .arg(sequence));
            // Still install the monitors below: if permission is granted later
            // without a restart, macOS activates them retroactively on some
            // versions, and this keeps state consistent either way.
        }

        NSEventMask mask = NSEventMaskKeyDown;
        globalMonitor_ = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
            handler:^(NSEvent* event) {
                handleEvent(event, /*local=*/false);
            }];
        localMonitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:mask
            handler:^NSEvent*(NSEvent* event) {
                handleEvent(event, /*local=*/true);
                return event;
            }];

        qCInfo(vtInput) << "addGlobalMonitorForEventsMatchingMask installed:"
                         << (globalMonitor_ != nil)
                         << "addLocalMonitorForEventsMatchingMask installed:"
                         << (localMonitor_ != nil);

        registered_ = true;
        qCInfo(vtInput) << "Registered global hotkey" << sequence;
        return true;
    }

    void unregisterHotkey() override {
        if (!registered_)
            return;
        if (globalMonitor_) {
            [NSEvent removeMonitor:globalMonitor_];
            globalMonitor_ = nil;
        }
        if (localMonitor_) {
            [NSEvent removeMonitor:localMonitor_];
            localMonitor_ = nil;
        }
        registered_ = false;
    }

private:
    // Logs only when the target key itself (e.g. V) is pressed anywhere, not
    // every keystroke system-wide — enough to diagnose delivery/matching
    // without writing a general keylogger to disk.
    void handleEvent(NSEvent* event, bool local) {
        if (!registered_ || event.keyCode != targetVk_)
            return;
        const NSEventModifierFlags mods =
            event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        const bool match = (mods == targetMods_);
        qCInfo(vtInput) << (local ? "Local" : "Global") << "candidate key event: keyCode="
                         << event.keyCode << "mods=" << mods << "target mods=" << targetMods_
                         << "match=" << match;
        if (!match)
            return;
        emit activated();
    }

    unsigned short targetVk_ = 0;
    NSEventModifierFlags targetMods_ = 0;
    id globalMonitor_ = nil;
    id localMonitor_ = nil;
    bool registered_ = false;
};

} // namespace

HotkeyService* HotkeyService::create(QObject* parent) {
    return new MacHotkeyService(parent);
}

} // namespace vt
