#include "core/FileLogging.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>

namespace vt {

namespace {

QtMessageHandler g_prevHandler = nullptr;
QFile g_logFile;
QMutex g_mutex;
std::atomic<bool> g_enabled{false};

const char* levelName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO ";
    case QtWarningMsg:
        return "WARN ";
    case QtCriticalMsg:
        return "CRIT ";
    case QtFatalMsg:
        return "FATAL";
    }
    return "?????";
}

void messageHandler(QtMsgType type, const QMessageLogContext& ctx,
                    const QString& msg) {
    // Write to the file first, *then* chain: the default handler aborts on a
    // fatal message, so doing it in this order guarantees the fatal line is
    // persisted before the process dies.
    if (g_enabled.load(std::memory_order_relaxed)) {
        QMutexLocker lock(&g_mutex);
        if (g_logFile.isOpen()) {
            const QByteArray line =
                (QDateTime::currentDateTime().toString(Qt::ISODateWithMs) +
                 QLatin1String(" ") + QLatin1String(levelName(type)) +
                 QLatin1String(" ") +
                 QLatin1String(ctx.category ? ctx.category : "default") +
                 QLatin1String(": ") + msg + QLatin1String("\n"))
                    .toUtf8();
            g_logFile.write(line);
            g_logFile.flush(); // push to the OS so a crash right after keeps it
        }
    }

    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

} // namespace

void installFileLogging(const QString& dir, bool enabled) {
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("voicetyper.log"));

    // Bound growth across runs: start fresh if the previous log got large. We
    // keep appending otherwise so a crash log survives the user's relaunch.
    if (QFileInfo(path).size() > 5 * 1024 * 1024)
        QFile::remove(path);

    g_logFile.setFileName(path);
    g_prevHandler = qInstallMessageHandler(messageHandler);
    setFileLoggingEnabled(enabled);
}

void setFileLoggingEnabled(bool enabled) {
    // NB: don't call qWarning()/qCWarning() while holding g_mutex — that would
    // re-enter messageHandler() and deadlock on this same (non-recursive) mutex.
    QMutexLocker lock(&g_mutex);
    if (enabled) {
        bool open = g_logFile.isOpen();
        if (!open && !g_logFile.fileName().isEmpty())
            open = g_logFile.open(QIODevice::Append | QIODevice::Text);
        g_enabled.store(open, std::memory_order_relaxed);
    } else {
        g_enabled.store(false, std::memory_order_relaxed);
        if (g_logFile.isOpen()) {
            g_logFile.flush();
            g_logFile.close();
        }
    }
}

} // namespace vt
