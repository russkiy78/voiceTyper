#pragma once

#include <QObject>
#include <QString>

namespace vt {

// Global (system-wide) hotkey registration. The hotkey fires regardless of
// which application has focus. Platform implementations live in
// HotkeyService_{x11,win,mac}.cpp; create() returns the right one.
class HotkeyService : public QObject {
    Q_OBJECT
public:
    explicit HotkeyService(QObject* parent = nullptr) : QObject(parent) {}
    ~HotkeyService() override = default;

    // Registers `sequence` (portable text, e.g. "Ctrl+Alt+Space"), replacing any
    // previously registered hotkey. Returns false on failure (and emits
    // registrationFailed with details).
    virtual bool setHotkey(const QString& sequence) = 0;
    virtual void unregisterHotkey() = 0;

    static HotkeyService* create(QObject* parent = nullptr);

signals:
    void activated();
    void registrationFailed(const QString& reason);
};

} // namespace vt
