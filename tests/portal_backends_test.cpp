// Exercises the Wayland portal backends (GlobalShortcuts hotkey, RemoteDesktop
// paste) against tests/mock_portal.py on a private session bus, so no real
// portal dialogs appear:
//   export XDG_CONFIG_HOME=$(mktemp -d) XDG_DATA_HOME=$(mktemp -d)
//   dbus-run-session -- sh -c 'python3 tests/mock_portal.py & sleep 1; ./portal_backends_test'
// Link Qt6Gui/DBus, X11, xcb, HotkeyParsing, HotkeyService_x11/_portal,
// KeyboardPaster_portal and XdgPortal, including moc for their QObject headers.
#include "clipboard/KeyboardPaster_portal.h"
#include "core/Logging.h"
#include "hotkey/HotkeyService_portal.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSettings>
#include <QThread>

#include <cstdio>
#include <functional>

Q_LOGGING_CATEGORY(vtInput, "voicetyper.input")

static bool waitFor(const std::function<bool()>& done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

static int check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("voiceTyper-test"));
    int failures = 0;

    auto* hotkey = new vt::PortalHotkeyService(&app);
    hotkey->setShortcutInfo(QStringLiteral("dictation"), QStringLiteral("Start or stop dictation"));
    int activations = 0;
    QObject::connect(hotkey, &vt::HotkeyService::activated, [&activations]() { ++activations; });
    failures += check(hotkey->setHotkey(QStringLiteral("Ctrl+Alt+V")), "setHotkey accepted");
    failures += check(waitFor([&]() { return activations > 0; }, 3000),
                      "hotkey activated on portal Deactivated");

    const auto token = []() {
        return QSettings().value(QStringLiteral("portal/remoteDesktopRestoreToken")).toString();
    };
    vt::PortalKeyboardPaster paster;
    paster.prepare();
    failures += check(waitFor([&]() { return paster.isReadyToPaste(); }, 3000), "session ready");
    failures += check(paster.sendPaste(), "first paste sent");
    failures += check(token() == QStringLiteral("tok1"), "restore token stored");

    // After the idle close the next paste reopens with the stored token.
    QThread::msleep(100);
    waitFor([]() { return false; }, 2500);
    paster.prepare();
    failures += check(waitFor([&]() { return paster.isReadyToPaste(); }, 3000),
                      "session reopened");
    failures += check(paster.sendPaste(), "second paste sent");
    failures += check(token() == QStringLiteral("tok2"), "fresh restore token stored");

    failures += check(activations == 1, "exactly one activation");
    return failures == 0 ? 0 : 1;
}
