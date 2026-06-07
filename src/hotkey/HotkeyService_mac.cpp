// macOS global hotkey — placeholder.
//
// TODO (macOS): implement with Carbon's RegisterEventHotKey + InstallEventHandler
// (still the standard way to get a system-wide hotkey on macOS), translating the
// ParsedHotkey to a Carbon virtual keycode + modifier mask. Requires the app to
// run as a proper .app bundle. Until then the hotkey is unavailable and the user
// must stop/start via the tray menu.

#include "hotkey/HotkeyService.h"

namespace vt {

namespace {

class MacHotkeyService final : public HotkeyService {
public:
    explicit MacHotkeyService(QObject* parent) : HotkeyService(parent) {}

    bool setHotkey(const QString& sequence) override {
        emit registrationFailed(
            tr("Global hotkey is not implemented on macOS yet (use the tray menu)."));
        Q_UNUSED(sequence);
        return false;
    }

    void unregisterHotkey() override {}
};

} // namespace

HotkeyService* HotkeyService::create(QObject* parent) {
    return new MacHotkeyService(parent);
}

} // namespace vt
