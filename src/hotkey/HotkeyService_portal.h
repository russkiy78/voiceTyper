#pragma once

#include "hotkey/HotkeyService.h"

#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

namespace vt {

// Global hotkeys on Wayland via the XDG GlobalShortcuts portal: the compositor
// owns the grab, asks the user once to confirm the shortcuts, and reports them
// over D-Bus whichever window has focus.
//
// A portal session accepts a single BindShortcuts call, so every hotkey of the
// process goes through this one object: changes are rebound together in a
// fresh session (one confirmation dialog, not one per hotkey).
class PortalShortcutSession : public QObject {
    Q_OBJECT
public:
    static PortalShortcutSession* instance();

    // `trigger` uses the XDG shortcut format, e.g. "CTRL+ALT+v".
    void setShortcut(const QString& id, const QString& description, const QString& trigger);
    void removeShortcut(const QString& id);

signals:
    // The shortcut was pressed and released again (activation on release, as
    // with the X11 grab).
    void released(const QString& id);
    void bindFailed(const QString& id, const QString& reason);

private slots:
    void onActivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                     const QVariantMap& options);
    void onDeactivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                       const QVariantMap& options);

private:
    struct Shortcut {
        QString description;
        QString trigger;
        bool operator==(const Shortcut&) const = default;
    };
    using Shortcuts = QMap<QString, Shortcut>;

    explicit PortalShortcutSession(QObject* parent);
    void scheduleRebind();
    void rebind();
    void bind(const Shortcuts& shortcuts);
    void finishBind(const Shortcuts& shortcuts, const QString& error);

    Shortcuts wanted_;
    Shortcuts requested_; // what the current session was asked to bind
    QString session_;
    bool busy_ = false;   // a CreateSession/BindShortcuts round trip is in flight
    bool scheduled_ = false;
    QSet<QString> pressed_;
};

class PortalHotkeyService final : public HotkeyService {
public:
    explicit PortalHotkeyService(QObject* parent);
    ~PortalHotkeyService() override;

    void setShortcutInfo(const QString& id, const QString& description) override;
    bool setHotkey(const QString& sequence) override;
    void unregisterHotkey() override;

private:
    QString id_ = QStringLiteral("hotkey");
    QString description_ = QStringLiteral("voiceTyper");
    bool registered_ = false;
};

} // namespace vt
