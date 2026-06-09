#include "app/AppController.h"
#include "core/FileLogging.h"
#include "core/Logging.h"

#include <QApplication>
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

// Logging category definitions (declared in core/Logging.h).
Q_LOGGING_CATEGORY(vtApp, "voicetyper.app")
Q_LOGGING_CATEGORY(vtAudio, "voicetyper.audio")
Q_LOGGING_CATEGORY(vtAsr, "voicetyper.asr")
Q_LOGGING_CATEGORY(vtCmd, "voicetyper.cmd")
Q_LOGGING_CATEGORY(vtInput, "voicetyper.input")
Q_LOGGING_CATEGORY(vtUi, "voicetyper.ui")

namespace {

// Local-socket name for the single-instance IPC channel. A second launch
// (`voiceTyper --toggle`, e.g. from a GNOME custom shortcut) connects here to
// drive the running instance. This is how the global hotkey is delivered on
// Wayland, where X11 key grabs never reach the app while a native-Wayland
// window is focused.
QString ipcServerName() { return QStringLiteral("voiceTyper.ipc"); }

// Hand a command to an already-running instance. Returns true if one accepted
// the connection (so this process should just exit).
bool forwardToRunningInstance(const QByteArray& command) {
    QLocalSocket socket;
    socket.connectToServer(ipcServerName());
    if (!socket.waitForConnected(300))
        return false;
    socket.write(command);
    socket.flush();
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QApplication::setOrganizationName(QStringLiteral("voiceTyper"));
    QApplication::setApplicationName(QStringLiteral("voiceTyper"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // Background tray utility: closing the settings window must not quit the app.
    QApplication::setQuitOnLastWindowClosed(false);

    const bool toggleRequested =
        QApplication::arguments().contains(QStringLiteral("--toggle"));

    // If an instance is already running, hand off and exit (single instance).
    if (forwardToRunningInstance(toggleRequested ? QByteArrayLiteral("toggle")
                                                 : QByteArrayLiteral("show")))
        return 0;

    // Install file logging before anything else runs (AppController loads the
    // ASR model — and may attempt a GPU init that hard-crashes — inside
    // initialize()). Flushed per line, this turns an otherwise-silent crash into
    // a log whose last entry pinpoints where it died. The key mirrors
    // SettingsStore's "logging/enabled" (default on).
    const bool loggingEnabled =
        QSettings().value(QStringLiteral("logging/enabled"), true).toBool();
    if (loggingEnabled) {
        // Promote our categories to info level so the diagnostic context (the
        // "Whisper compute backend: ..." line logged right before a GPU init)
        // actually reaches the log; Qt disables info by default. Debug stays
        // off. A user's QT_LOGGING_RULES env still overrides this.
        QLoggingCategory::setFilterRules(QStringLiteral("voicetyper.*.info=true"));
    }
    vt::installFileLogging(
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
        loggingEnabled);

    vt::AppController controller;
    if (!controller.initialize())
        return 1;

    // Single-instance IPC server: an incoming "toggle" starts/stops recording,
    // letting an external GNOME shortcut act as the global hotkey on Wayland.
    QLocalServer ipcServer;
    QLocalServer::removeServer(ipcServerName()); // clear a stale socket file
    if (!ipcServer.listen(ipcServerName()))
        qCWarning(vtApp) << "IPC server failed to listen:" << ipcServer.errorString();

    QObject::connect(&ipcServer, &QLocalServer::newConnection, &controller,
                     [&ipcServer, &controller] {
                         QLocalSocket* conn = ipcServer.nextPendingConnection();
                         if (!conn)
                             return;
                         if (conn->waitForReadyRead(300) &&
                             conn->readAll().contains("toggle"))
                             controller.toggleRecording();
                         conn->disconnectFromServer();
                         conn->deleteLater();
                     });

    // Launched with --toggle while nothing was running: start dictation now so
    // the first shortcut press also begins recording.
    if (toggleRequested)
        QMetaObject::invokeMethod(&controller, "toggleRecording",
                                  Qt::QueuedConnection);

    return QApplication::exec();
}
