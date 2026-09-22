#pragma once

// Minimal client for the XDG desktop portal (org.freedesktop.portal.*). On a
// Wayland session the compositor owns global input: X11 key grabs only fire
// while an Xwayland window is focused, and XTEST reaches native Wayland windows
// only through a portal session Xwayland re-opens (and GNOME re-confirms) for
// every new burst of events. The portal-based hotkey and paste backends use
// these helpers instead. Linux only.

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace vt::portal {

inline const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
inline const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");

// The app id we register with the portal. GNOME only honours a reverse-DNS id
// backed by an installed <id>.desktop file (see ensureDesktopFile()).
inline const QString kAppId = QStringLiteral("io.github.russkiy78.voiceTyper");

// True when global input has to go through the portal: a Wayland session whose
// X server, if any, is Xwayland (a real X server, e.g. Xvfb in tests, keeps the
// X11 backends).
bool waylandInput();

// True if the portal implements `interface`, e.g.
// "org.freedesktop.portal.GlobalShortcuts".
bool hasInterface(const QString& interface);

// Private session-bus connection for all our portal calls, registered under
// kAppId on first use. Registration only works as the first portal call on a
// connection, and Qt's platform theme already queries the Settings portal on
// the shared one.
QDBusConnection bus();

// A unique token for handle_token / session_handle_token options.
QString newToken();

// The session_handle of a CreateSession response (typed "s" or "o").
QString sessionHandle(const QVariantMap& results);

void closeSession(const QString& session);

// One portal call answered through an org.freedesktop.portal.Request object.
// `done` runs once with the Response code (0 success, 1 cancelled by the user,
// 2 other error) unless `context` is destroyed first.
class Request : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(uint response, const QVariantMap& results)>;

    // `args` are the method's leading arguments; `options` is its trailing
    // a{sv}, to which a handle_token is added.
    static void send(const QString& interface, const QString& method, QVariantList args,
                     QVariantMap options, QObject* context, Callback done);

private slots:
    void onResponse(uint response, const QVariantMap& results);

private:
    Request(QObject* context, QString path, Callback done);
    void finish(uint response, const QVariantMap& results);

    QString path_;
    Callback done_;
};

} // namespace vt::portal
