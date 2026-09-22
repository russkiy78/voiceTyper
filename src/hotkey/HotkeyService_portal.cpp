// Wayland global hotkeys via the XDG GlobalShortcuts portal; see
// HotkeyService_portal.h.

#include "hotkey/HotkeyService_portal.h"

#include "core/Logging.h"
#include "core/XdgPortal.h"
#include "hotkey/HotkeyParsing.h"
#include "hotkey/X11Keysym.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <X11/Xlib.h>

namespace vt {

// One entry of the portal's a(sa{sv}) shortcut lists.
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};

QDBusArgument& operator<<(QDBusArgument& arg, const PortalShortcut& shortcut) {
    arg.beginStructure();
    arg << shortcut.id << shortcut.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalShortcut& shortcut) {
    arg.beginStructure();
    arg >> shortcut.id >> shortcut.properties;
    arg.endStructure();
    return arg;
}

} // namespace vt

Q_DECLARE_METATYPE(vt::PortalShortcut)

namespace vt {

namespace {

const QString kInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

// "Ctrl+Alt+V" -> "CTRL+ALT+v": XDG shortcuts-spec modifiers, then the xkb
// keysym name. Empty if the key has no keysym.
QString portalTrigger(const ParsedHotkey& hk) {
    const unsigned long keysym = qtKeyToKeysym(hk.key);
    const char* name = keysym ? XKeysymToString(keysym) : nullptr;
    if (!name)
        return {};
    QString key = QString::fromLatin1(name);
    if (key.size() == 1)
        key = key.toLower(); // "V" names the shifted keysym

    QStringList parts;
    if (hk.modifiers & Qt::ControlModifier)
        parts << QStringLiteral("CTRL");
    if (hk.modifiers & Qt::AltModifier)
        parts << QStringLiteral("ALT");
    if (hk.modifiers & Qt::ShiftModifier)
        parts << QStringLiteral("SHIFT");
    if (hk.modifiers & Qt::MetaModifier)
        parts << QStringLiteral("LOGO");
    parts << key;
    return parts.join(QLatin1Char('+'));
}

} // namespace

PortalShortcutSession* PortalShortcutSession::instance() {
    static QPointer<PortalShortcutSession> session;
    if (!session)
        session = new PortalShortcutSession(qApp);
    return session;
}

PortalShortcutSession::PortalShortcutSession(QObject* parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();

    QDBusConnection bus = portal::bus();
    bus.connect(portal::kService, portal::kPath, kInterface, QStringLiteral("Activated"), this,
                SLOT(onActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    bus.connect(portal::kService, portal::kPath, kInterface, QStringLiteral("Deactivated"), this,
                SLOT(onDeactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    qCInfo(vtInput) << "Global hotkeys via the XDG GlobalShortcuts portal";
}

void PortalShortcutSession::setShortcut(const QString& id, const QString& description,
                                        const QString& trigger) {
    wanted_.insert(id, {description, trigger});
    scheduleRebind();
}

void PortalShortcutSession::removeShortcut(const QString& id) {
    if (wanted_.remove(id))
        scheduleRebind();
}

void PortalShortcutSession::scheduleRebind() {
    // Hotkeys set back to back (dictation + translation) share one bind.
    if (scheduled_)
        return;
    scheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        scheduled_ = false;
        rebind();
    });
}

void PortalShortcutSession::rebind() {
    if (busy_ || wanted_ == requested_)
        return; // when busy, finishBind() checks again

    if (!session_.isEmpty()) {
        portal::closeSession(session_);
        session_.clear();
    }
    pressed_.clear();
    requested_ = wanted_;
    if (wanted_.isEmpty())
        return;

    busy_ = true;
    const Shortcuts shortcuts = wanted_;
    portal::Request::send(
        kInterface, QStringLiteral("CreateSession"), {},
        {{QStringLiteral("session_handle_token"), portal::newToken()}}, this,
        [this, shortcuts](uint response, const QVariantMap& results) {
            const QString session = portal::sessionHandle(results);
            if (response != 0 || session.isEmpty()) {
                finishBind(shortcuts, tr("Could not open a global shortcuts session."));
                return;
            }
            session_ = session;
            bind(shortcuts);
        });
}

void PortalShortcutSession::bind(const Shortcuts& shortcuts) {
    QList<PortalShortcut> list;
    for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it) {
        list.append({it.key(),
                     {{QStringLiteral("description"), it->description},
                      {QStringLiteral("preferred_trigger"), it->trigger}}});
    }

    // The first bind makes GNOME ask the user to confirm the shortcuts; the
    // answer is remembered for our app id.
    portal::Request::send(
        kInterface, QStringLiteral("BindShortcuts"),
        {QVariant::fromValue(QDBusObjectPath(session_)), QVariant::fromValue(list), QString()},
        {}, this, [this, shortcuts](uint response, const QVariantMap& results) {
            if (response != 0) {
                finishBind(shortcuts, response == 1
                                          ? tr("The global shortcut was not allowed.")
                                          : tr("Could not bind the global shortcut."));
                return;
            }
            const QVariant bound = results.value(QStringLiteral("shortcuts"));
            if (bound.metaType() == QMetaType::fromType<QDBusArgument>()) {
                QList<PortalShortcut> granted;
                bound.value<QDBusArgument>() >> granted;
                for (const PortalShortcut& s : granted) {
                    qCInfo(vtInput) << "Global shortcut" << s.id << "bound:"
                                    << s.properties.value(QStringLiteral("trigger_description"))
                                           .toString();
                }
            }
            finishBind(shortcuts, {});
        });
}

void PortalShortcutSession::finishBind(const Shortcuts& shortcuts, const QString& error) {
    busy_ = false;
    if (!error.isEmpty()) {
        qCWarning(vtInput) << "Portal shortcut binding failed:" << error;
        if (!session_.isEmpty()) {
            portal::closeSession(session_);
            session_.clear();
        }
        for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it)
            emit bindFailed(it.key(), error);
    }
    if (wanted_ != requested_)
        scheduleRebind();
}

void PortalShortcutSession::onActivated(const QDBusObjectPath& session, const QString& id,
                                        qulonglong, const QVariantMap&) {
    if (session.path() == session_)
        pressed_.insert(id);
}

void PortalShortcutSession::onDeactivated(const QDBusObjectPath& session, const QString& id,
                                          qulonglong, const QVariantMap&) {
    // Fire on release, like the X11 grab: a paste synthesized while the
    // hotkey's modifiers are still held would itself look like the hotkey.
    if (session.path() == session_ && pressed_.remove(id))
        emit released(id);
}

PortalHotkeyService::PortalHotkeyService(QObject* parent) : HotkeyService(parent) {
    auto* session = PortalShortcutSession::instance();
    connect(session, &PortalShortcutSession::released, this, [this](const QString& id) {
        if (registered_ && id == id_)
            emit activated();
    });
    connect(session, &PortalShortcutSession::bindFailed, this,
            [this](const QString& id, const QString& reason) {
                if (id == id_)
                    emit registrationFailed(reason);
            });
}

PortalHotkeyService::~PortalHotkeyService() {
    unregisterHotkey();
}

void PortalHotkeyService::setShortcutInfo(const QString& id, const QString& description) {
    id_ = id;
    description_ = description;
}

bool PortalHotkeyService::setHotkey(const QString& sequence) {
    const ParsedHotkey hk = parseHotkey(sequence);
    if (!hk.valid) {
        emit registrationFailed(tr("Could not parse hotkey '%1'.").arg(sequence));
        return false;
    }
    const QString trigger = portalTrigger(hk);
    if (trigger.isEmpty()) {
        emit registrationFailed(tr("Unsupported key in '%1'.").arg(sequence));
        return false;
    }
    PortalShortcutSession::instance()->setShortcut(id_, description_, trigger);
    registered_ = true;
    return true;
}

void PortalHotkeyService::unregisterHotkey() {
    if (!registered_)
        return;
    registered_ = false;
    PortalShortcutSession::instance()->removeShortcut(id_);
}

} // namespace vt
