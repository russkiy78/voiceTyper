#include "core/XdgPortal.h"

#include "core/Logging.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <X11/Xlib.h>

#include <algorithm>

namespace vt::portal {

namespace {

const QString kRequestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString kConnectionName = QStringLiteral("voicetyper-portal");
const QByteArray kGeneratedMarker = QByteArrayLiteral("X-voiceTyper-Generated=true");

// GNOME's portal backend rejects an app id without an installed desktop file.
// Packages ship one; for a binary run from anywhere else, keep an entry
// pointing at it in the user's applications directory. It stays visible:
// Settings → Apps is where GNOME lets the user edit the app's global shortcuts.
void ensureDesktopFile() {
    const QString name = kAppId + QStringLiteral(".desktop");
    const QString own =
        QDir(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation))
            .filePath(name);
    QByteArray current;
    if (QFile f(own); f.open(QIODevice::ReadOnly))
        current = f.readAll();
    const bool generated = current.contains(kGeneratedMarker);

    const QStringList installed =
        QStandardPaths::locateAll(QStandardPaths::ApplicationsLocation, name);
    const bool packaged = std::any_of(installed.cbegin(), installed.cend(),
                                      [&own](const QString& path) { return path != own; });
    if (packaged) {
        if (generated)
            QFile::remove(own); // don't shadow the packaged entry
        return;
    }
    if (!current.isEmpty() && !generated)
        return; // the user's own entry

    const QByteArray entry = QStringLiteral("[Desktop Entry]\n"
                                            "Type=Application\n"
                                            "Name=voiceTyper\n"
                                            "Comment=Local voice typing\n"
                                            "Exec=\"%1\"\n"
                                            "Icon=voicetyper\n"
                                            "Categories=Utility;Accessibility;\n")
                                 .arg(QCoreApplication::applicationFilePath())
                                 .toUtf8() +
                             kGeneratedMarker + '\n';
    if (entry == current)
        return;
    QDir().mkpath(QFileInfo(own).path());
    QFile out(own);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(vtInput) << "Cannot write" << own << ":" << out.errorString();
        return;
    }
    out.write(entry);
    qCInfo(vtInput) << "Wrote" << own << "so the desktop portal knows voiceTyper";
}

} // namespace

bool waylandInput() {
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        return false;
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return true;
    int opcode = 0, event = 0, error = 0;
    const bool xwayland = XQueryExtension(display, "XWAYLAND", &opcode, &event, &error);
    XCloseDisplay(display);
    return xwayland;
}

bool hasInterface(const QString& interface) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    msg << interface << QStringLiteral("version");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

QDBusConnection bus() {
    static bool connected = false;
    if (connected)
        return QDBusConnection(kConnectionName);
    connected = true;

    QDBusConnection connection =
        QDBusConnection::connectToBus(QDBusConnection::SessionBus, kConnectionName);
    ensureDesktopFile();
    QDBusMessage msg = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.host.portal.Registry"),
        QStringLiteral("Register"));
    msg << kAppId << QVariantMap();
    const QDBusMessage reply = connection.call(msg);
    if (reply.type() == QDBusMessage::ErrorMessage)
        qCWarning(vtInput) << "Portal app id registration failed:" << reply.errorMessage();
    return connection;
}

QString newToken() {
    static uint counter = 0;
    return QStringLiteral("voicetyper%1").arg(++counter);
}

QString sessionHandle(const QVariantMap& results) {
    const QVariant v = results.value(QStringLiteral("session_handle"));
    if (v.metaType() == QMetaType::fromType<QDBusObjectPath>())
        return v.value<QDBusObjectPath>().path();
    return v.toString();
}

void closeSession(const QString& session) {
    const QDBusMessage msg = QDBusMessage::createMethodCall(
        kService, session, QStringLiteral("org.freedesktop.portal.Session"),
        QStringLiteral("Close"));
    bus().call(msg, QDBus::NoBlock);
}

Request::Request(QObject* context, QString path, Callback done)
    : QObject(context), path_(std::move(path)), done_(std::move(done)) {}

void Request::send(const QString& interface, const QString& method, QVariantList args,
                   QVariantMap options, QObject* context, Callback done) {
    QDBusConnection connection = bus();
    const QString token = newToken();

    // The portal derives the Request path from our unique name and the token,
    // so subscribe before calling: a fast Response can't be missed.
    const QString sender =
        connection.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString path =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    auto* request = new Request(context, path, std::move(done));
    connection.connect(kService, path, kRequestInterface, QStringLiteral("Response"), request,
                       SLOT(onResponse(uint,QVariantMap)));

    options.insert(QStringLiteral("handle_token"), token);
    args << options;
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, interface, method);
    msg.setArguments(args);
    const QDBusMessage reply = connection.call(msg);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(vtInput) << "Portal" << method << "failed:" << reply.errorMessage();
        request->finish(2, {});
    }
}

void Request::onResponse(uint response, const QVariantMap& results) {
    finish(response, results);
}

void Request::finish(uint response, const QVariantMap& results) {
    bus().disconnect(kService, path_, kRequestInterface, QStringLiteral("Response"), this,
                     SLOT(onResponse(uint,QVariantMap)));
    const Callback done = std::move(done_);
    deleteLater();
    if (done)
        done(response, results);
}

} // namespace vt::portal
