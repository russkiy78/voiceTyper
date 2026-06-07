// Windows global hotkey via RegisterHotKey + WM_HOTKEY.

#include "hotkey/HotkeyService.h"

#include "core/Logging.h"
#include "hotkey/HotkeyParsing.h"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>

#include <windows.h>

namespace vt {

namespace {

constexpr int kHotkeyId = 0xB001;

UINT qtKeyToVk(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));

    switch (key) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    default: return 0;
    }
}

UINT qtModsToWin(Qt::KeyboardModifiers mods) {
    UINT m = 0;
    if (mods & Qt::AltModifier) m |= MOD_ALT;
    if (mods & Qt::ControlModifier) m |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier) m |= MOD_SHIFT;
    if (mods & Qt::MetaModifier) m |= MOD_WIN;
    return m;
}

class WinHotkeyService final : public HotkeyService,
                              public QAbstractNativeEventFilter {
public:
    explicit WinHotkeyService(QObject* parent) : HotkeyService(parent) {
        qApp->installNativeEventFilter(this);
    }
    ~WinHotkeyService() override {
        unregisterHotkey();
        qApp->removeNativeEventFilter(this);
    }

    bool setHotkey(const QString& sequence) override {
        const ParsedHotkey hk = parseHotkey(sequence);
        if (!hk.valid) {
            emit registrationFailed(tr("Could not parse hotkey '%1'.").arg(sequence));
            return false;
        }
        const UINT vk = qtKeyToVk(hk.key);
        if (vk == 0) {
            emit registrationFailed(tr("Unsupported key in '%1'.").arg(sequence));
            return false;
        }

        unregisterHotkey();

        const UINT mods = qtModsToWin(hk.modifiers) | MOD_NOREPEAT;
        if (!RegisterHotKey(nullptr, kHotkeyId, mods, vk)) {
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
        if (!registered_)
            return;
        UnregisterHotKey(nullptr, kHotkeyId);
        registered_ = false;
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr*) override {
        if (!registered_ || eventType != "windows_generic_MSG")
            return false;
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == kHotkeyId)
            emit activated();
        return false;
    }

private:
    bool registered_ = false;
};

} // namespace

HotkeyService* HotkeyService::create(QObject* parent) {
    return new WinHotkeyService(parent);
}

} // namespace vt
