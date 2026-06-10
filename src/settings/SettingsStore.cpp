#include "settings/SettingsStore.h"

#include "core/Logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace vt {

namespace {
namespace keys {
constexpr auto kLanguage = "asr/language";
constexpr auto kTranslate = "asr/translate";
constexpr auto kThreads = "asr/threads";
constexpr auto kModelPath = "asr/modelPath";
constexpr auto kComputeBackend = "asr/computeBackend";
constexpr auto kLoadPendingPath = "asr/loadPendingPath";
constexpr auto kLoadPendingGpu = "asr/loadPendingGpu";
constexpr auto kLoggingEnabled = "logging/enabled";

constexpr auto kHotkey = "hotkey/sequence";
constexpr auto kTranslateHotkey = "hotkey/translateSequence";
constexpr auto kOverlay = "overlay/enabled";
constexpr auto kClipboardDelay = "clipboard/restoreDelayMs";
constexpr auto kCdEnabled = "commandDetection/enabled";
constexpr auto kCdInterval = "commandDetection/intervalMs";
constexpr auto kCdWindow = "commandDetection/windowSeconds";
constexpr auto kPpEnabled = "postProcess/enabled";
constexpr auto kPpEndpoint = "postProcess/endpoint";
} // namespace keys
} // namespace

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {
    QDir().mkpath(configDir());
}

QString SettingsStore::language() const {
    return QSettings().value(keys::kLanguage, "auto").toString();
}
void SettingsStore::setLanguage(const QString& code) {
    QSettings().setValue(keys::kLanguage, code);
    emit changed();
}

bool SettingsStore::translate() const {
    return QSettings().value(keys::kTranslate, false).toBool();
}
void SettingsStore::setTranslate(bool on) {
    QSettings().setValue(keys::kTranslate, on);
    emit changed();
}

int SettingsStore::threads() const {
    return QSettings().value(keys::kThreads, 0).toInt();
}
void SettingsStore::setThreads(int n) {
    QSettings().setValue(keys::kThreads, n);
    emit changed();
}

QString SettingsStore::modelPath() const {
    const QString stored = QSettings().value(keys::kModelPath).toString();
    if (!stored.isEmpty())
        return stored;
    return autodetectModelPath();
}
void SettingsStore::setModelPath(const QString& path) {
    QSettings s;
    s.setValue(keys::kModelPath, path);
    emit changed();
}

QString SettingsStore::computeBackend() const {
    return QSettings().value(keys::kComputeBackend, QString()).toString();
}
void SettingsStore::setComputeBackend(const QString& id) {
    QSettings().setValue(keys::kComputeBackend, id);
    emit changed();
}

QString SettingsStore::loadAttemptPath() const {
    return QSettings().value(keys::kLoadPendingPath).toString();
}
bool SettingsStore::loadAttemptWasGpu() const {
    return QSettings().value(keys::kLoadPendingGpu, false).toBool();
}
void SettingsStore::setLoadAttempt(const QString& modelPath, bool gpu) {
    QSettings s;
    s.setValue(keys::kLoadPendingPath, modelPath);
    s.setValue(keys::kLoadPendingGpu, gpu);
    s.sync(); // flush now: the very next step may crash the whole process
}
void SettingsStore::clearLoadAttempt() {
    QSettings s;
    s.remove(keys::kLoadPendingPath);
    s.remove(keys::kLoadPendingGpu);
    s.sync();
}



QString SettingsStore::hotkey() const {
    return QSettings().value(keys::kHotkey, "Ctrl+Alt+V").toString();
}
void SettingsStore::setHotkey(const QString& seq) {
    QSettings().setValue(keys::kHotkey, seq);
    emit changed();
}

QString SettingsStore::translateHotkey() const {
    return QSettings().value(keys::kTranslateHotkey, "Ctrl+Alt+;").toString();
}
void SettingsStore::setTranslateHotkey(const QString& seq) {
    QSettings().setValue(keys::kTranslateHotkey, seq);
    emit changed();
}

bool SettingsStore::overlayEnabled() const {
    return QSettings().value(keys::kOverlay, true).toBool();
}
void SettingsStore::setOverlayEnabled(bool on) {
    QSettings().setValue(keys::kOverlay, on);
    emit changed();
}

int SettingsStore::clipboardRestoreDelayMs() const {
    return QSettings().value(keys::kClipboardDelay, 600).toInt();
}
void SettingsStore::setClipboardRestoreDelayMs(int ms) {
    QSettings().setValue(keys::kClipboardDelay, ms);
    emit changed();
}

bool SettingsStore::commandDetectionEnabled() const {
    return QSettings().value(keys::kCdEnabled, true).toBool();
}
void SettingsStore::setCommandDetectionEnabled(bool on) {
    QSettings().setValue(keys::kCdEnabled, on);
    emit changed();
}

int SettingsStore::commandDetectionIntervalMs() const {
    return QSettings().value(keys::kCdInterval, 2000).toInt();
}
void SettingsStore::setCommandDetectionIntervalMs(int ms) {
    QSettings().setValue(keys::kCdInterval, ms);
    emit changed();
}

double SettingsStore::commandDetectionWindowSeconds() const {
    return QSettings().value(keys::kCdWindow, 4.0).toDouble();
}
void SettingsStore::setCommandDetectionWindowSeconds(double s) {
    QSettings().setValue(keys::kCdWindow, s);
    emit changed();
}

bool SettingsStore::postProcessEnabled() const {
    return QSettings().value(keys::kPpEnabled, false).toBool();
}
void SettingsStore::setPostProcessEnabled(bool on) {
    QSettings().setValue(keys::kPpEnabled, on);
    emit changed();
}

QString SettingsStore::postProcessEndpoint() const {
    return QSettings().value(keys::kPpEndpoint).toString();
}
void SettingsStore::setPostProcessEndpoint(const QString& url) {
    QSettings().setValue(keys::kPpEndpoint, url);
    emit changed();
}

bool SettingsStore::loggingEnabled() const {
    return QSettings().value(keys::kLoggingEnabled, true).toBool();
}
void SettingsStore::setLoggingEnabled(bool on) {
    QSettings().setValue(keys::kLoggingEnabled, on);
    emit changed();
}

QString SettingsStore::configDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString SettingsStore::commandsConfigPath() const {
    return QDir(configDir()).filePath("commands.json");
}

QString SettingsStore::bundledDefaultCommandsPath() const {
    const QString next =
        QDir(QCoreApplication::applicationDirPath()).filePath("commands.default.json");
    if (QFileInfo::exists(next))
        return next;
    // Fallback to a source-tree relative path for dev runs.
    return QStringLiteral("config/commands.default.json");
}

QString SettingsStore::loadCommandsJson() const {
    const QString path = commandsConfigPath();

    if (!QFileInfo::exists(path)) {
        // Materialize the bundled default on first run.
        QFile def(bundledDefaultCommandsPath());
        if (def.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray data = def.readAll();
            def.close();
            QFile out(path);
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                out.write(data);
                out.close();
            }
            return QString::fromUtf8(data);
        }
        qCWarning(vtApp) << "No commands.json and no bundled default found";
        return QStringLiteral("{\n  \"commands\": []\n}\n");
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(vtApp) << "Failed to open commands config" << path;
        return QStringLiteral("{\n  \"commands\": []\n}\n");
    }
    const QString contents = QString::fromUtf8(f.readAll());
    f.close();
    return contents;
}

bool SettingsStore::saveCommandsJson(const QString& json, QString* error) {
    const QString path = commandsConfigPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(json.toUtf8());
    f.close();
    emit changed();
    return true;
}

QString SettingsStore::autodetectModelPath() {
    QStringList searchDirs;
    searchDirs << QDir(QCoreApplication::applicationDirPath()).filePath("models")
               << QCoreApplication::applicationDirPath()
               << QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                      .filePath("models")
               << QStringLiteral("models");

    const QStringList patterns{"ggml-*.bin", "*.bin", "*.gguf"};
    for (const QString& dir : searchDirs) {
        QDir d(dir);
        if (!d.exists())
            continue;
        const QStringList found = d.entryList(patterns, QDir::Files, QDir::Size);
        if (!found.isEmpty())
            return d.filePath(found.first());
    }
    return {};
}

} // namespace vt
