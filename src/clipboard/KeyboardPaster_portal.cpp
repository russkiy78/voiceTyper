// Wayland paste keystroke via the XDG RemoteDesktop portal; see
// KeyboardPaster_portal.h.

#include "clipboard/KeyboardPaster_portal.h"

#include "core/Logging.h"
#include "core/XdgPortal.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QSettings>

namespace vt {

namespace {

const QString kInterface = QStringLiteral("org.freedesktop.portal.RemoteDesktop");
const QString kSessionInterface = QStringLiteral("org.freedesktop.portal.Session");
constexpr auto kRestoreTokenKey = "portal/remoteDesktopRestoreToken";

constexpr uint kKeyboardDevice = 1;
constexpr uint kPersistUntilRevoked = 2;

// evdev codes of the physical keys, so Ctrl+V still pastes while a non-Latin
// layout is active (a "v" keysym has no key there).
constexpr int kKeyLeftCtrl = 29;
constexpr int kKeyV = 47;

// Reopening with a restore token is silent and quick; keep the session a
// moment for back-to-back pastes, then drop it (and GNOME's indicator).
constexpr int kIdleCloseMs = 2000;

} // namespace

PortalKeyboardPaster::PortalKeyboardPaster() {
    idleClose_.setSingleShot(true);
    idleClose_.setInterval(kIdleCloseMs);
    connect(&idleClose_, &QTimer::timeout, this, &PortalKeyboardPaster::close);
    qCInfo(vtInput) << "Paste keystroke via the XDG RemoteDesktop portal";
}

PortalKeyboardPaster::~PortalKeyboardPaster() {
    close();
}

void PortalKeyboardPaster::prepare() {
    idleClose_.stop();
    if (state_ == State::Starting || state_ == State::Ready)
        return;

    state_ = State::Starting;
    portal::Request::send(
        kInterface, QStringLiteral("CreateSession"), {},
        {{QStringLiteral("session_handle_token"), portal::newToken()}}, this,
        [this](uint response, const QVariantMap& results) {
            session_ = portal::sessionHandle(results);
            if (response != 0 || session_.isEmpty()) {
                fail(QStringLiteral("CreateSession response %1").arg(response));
                return;
            }
            portal::bus().connect(portal::kService, session_, kSessionInterface,
                                  QStringLiteral("Closed"), this,
                                  SLOT(onSessionClosed(QVariantMap)));
            selectDevices();
        });
}

void PortalKeyboardPaster::selectDevices() {
    QVariantMap options{{QStringLiteral("types"), kKeyboardDevice},
                        {QStringLiteral("persist_mode"), kPersistUntilRevoked}};
    const QString token = QSettings().value(kRestoreTokenKey).toString();
    if (!token.isEmpty())
        options.insert(QStringLiteral("restore_token"), token);

    portal::Request::send(kInterface, QStringLiteral("SelectDevices"),
                          {QVariant::fromValue(QDBusObjectPath(session_))}, options, this,
                          [this](uint response, const QVariantMap&) {
                              if (response != 0) {
                                  fail(QStringLiteral("SelectDevices response %1").arg(response));
                                  return;
                              }
                              start();
                          });
}

void PortalKeyboardPaster::start() {
    // Without a valid restore token GNOME asks the user here.
    portal::Request::send(
        kInterface, QStringLiteral("Start"),
        {QVariant::fromValue(QDBusObjectPath(session_)), QString()}, {}, this,
        [this](uint response, const QVariantMap& results) {
            if (response != 0 ||
                !(results.value(QStringLiteral("devices")).toUInt() & kKeyboardDevice)) {
                fail(QStringLiteral("Start response %1").arg(response));
                return;
            }
            // Tokens are single use: keep the fresh one for the next session.
            const QString token = results.value(QStringLiteral("restore_token")).toString();
            persistent_ = !token.isEmpty();
            if (persistent_)
                QSettings().setValue(kRestoreTokenKey, token);
            else
                QSettings().remove(kRestoreTokenKey);
            state_ = State::Ready;
        });
}

bool PortalKeyboardPaster::isReadyToPaste() const {
    return state_ != State::Starting;
}

bool PortalKeyboardPaster::sendPaste() {
    if (state_ != State::Ready)
        return false;

    // Send every event even if one fails, so Ctrl can't be left held down.
    bool ok = sendKey(kKeyLeftCtrl, true);
    ok = sendKey(kKeyV, true) && ok;
    ok = sendKey(kKeyV, false) && ok;
    ok = sendKey(kKeyLeftCtrl, false) && ok;

    if (persistent_)
        idleClose_.start();
    return ok;
}

bool PortalKeyboardPaster::sendKey(int evdevCode, bool pressed) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        portal::kService, portal::kPath, kInterface, QStringLiteral("NotifyKeyboardKeycode"));
    msg << QVariant::fromValue(QDBusObjectPath(session_)) << QVariantMap() << evdevCode
        << uint(pressed ? 1 : 0);
    const QDBusMessage reply = portal::bus().call(msg);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(vtInput) << "Portal key event failed:" << reply.errorMessage();
        return false;
    }
    return true;
}

void PortalKeyboardPaster::fail(const QString& reason) {
    qCWarning(vtInput) << "Remote desktop portal session failed:" << reason;
    close();
    state_ = State::Failed;
}

void PortalKeyboardPaster::close() {
    idleClose_.stop();
    if (!session_.isEmpty()) {
        portal::bus().disconnect(portal::kService, session_, kSessionInterface,
                                 QStringLiteral("Closed"), this,
                                 SLOT(onSessionClosed(QVariantMap)));
        portal::closeSession(session_);
        session_.clear();
    }
    state_ = State::Idle;
}

void PortalKeyboardPaster::onSessionClosed(const QVariantMap&) {
    // Ended by the desktop (e.g. "Stop" on the sharing indicator); the next
    // paste opens a new one.
    portal::bus().disconnect(portal::kService, session_, kSessionInterface,
                             QStringLiteral("Closed"), this, SLOT(onSessionClosed(QVariantMap)));
    session_.clear();
    idleClose_.stop();
    state_ = State::Idle;
}

} // namespace vt
