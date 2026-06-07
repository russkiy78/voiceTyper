#pragma once

#include <QObject>
#include <QString>

namespace vt {

// Persists user configuration via QSettings and owns the on-disk commands JSON.
//
// QSettings location follows QCoreApplication::organizationName/applicationName
// (set in main()). The commands config lives as a standalone JSON file under the
// app config directory so the user (or the Settings UI) can edit it directly.
class SettingsStore : public QObject {
    Q_OBJECT
public:
    explicit SettingsStore(QObject* parent = nullptr);

    // --- ASR -------------------------------------------------------------
    QString language() const;            // "auto", "ru", "en", ...
    void setLanguage(const QString& code);

    bool translate() const;              // translate to English
    void setTranslate(bool on);

    int threads() const;                 // 0 => auto
    void setThreads(int n);

    QString modelPath() const;           // path to ggml-*.bin
    void setModelPath(const QString& path);

    // Compute backend id: "cpu", "vulkan", "cuda", ... or empty/"auto" (prefer
    // GPU when one is available). Resolved at runtime by ComputeBackends.
    QString computeBackend() const;
    void setComputeBackend(const QString& id);



    // --- Hotkey ----------------------------------------------------------
    QString hotkey() const;              // e.g. "Ctrl+Alt+Space"
    void setHotkey(const QString& seq);

    // --- Overlay ---------------------------------------------------------
    bool overlayEnabled() const;
    void setOverlayEnabled(bool on);

    // --- Clipboard / paste ----------------------------------------------
    int clipboardRestoreDelayMs() const; // delay before restoring old clipboard
    void setClipboardRestoreDelayMs(int ms);

    // --- Command detection loop -----------------------------------------
    bool commandDetectionEnabled() const;
    void setCommandDetectionEnabled(bool on);

    int commandDetectionIntervalMs() const;     // how often to sample
    void setCommandDetectionIntervalMs(int ms);

    double commandDetectionWindowSeconds() const; // tail length analysed
    void setCommandDetectionWindowSeconds(double s);

    // --- Post-processing (future HTTPS cleanup) -------------------------
    bool postProcessEnabled() const;
    void setPostProcessEnabled(bool on);

    QString postProcessEndpoint() const;
    void setPostProcessEndpoint(const QString& url);

    // --- Commands config file -------------------------------------------
    QString configDir() const;
    QString commandsConfigPath() const;

    // Reads the commands JSON, materializing the bundled default on first run.
    QString loadCommandsJson() const;
    // Writes the commands JSON to disk. Returns false and sets *error on failure.
    bool saveCommandsJson(const QString& json, QString* error = nullptr);

    // Best-effort scan for a bundled model if none is configured.
    static QString autodetectModelPath();

signals:
    void changed();

private:
    QString bundledDefaultCommandsPath() const;
};

} // namespace vt
