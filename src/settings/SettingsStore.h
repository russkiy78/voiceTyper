#pragma once

#include <QObject>
#include <QString>

namespace vt {

// Persists user configuration via QSettings and owns the on-disk commands and
// initial-prompts JSON files.
//
// QSettings location follows QCoreApplication::organizationName/applicationName
// (set in main()). The commands and prompts configs live as standalone JSON
// files under the app config directory so the user (or the Settings UI) can
// edit them directly.
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

    // Decode only the speech the bundled Silero VAD detects. Off => whole
    // recordings are decoded, silence included. Read per dictation.
    bool vadEnabled() const;
    void setVadEnabled(bool on);

    QString modelPath() const;           // path to ggml-*.bin
    void setModelPath(const QString& path);

    // Compute backend id: "cpu", "vulkan", "cuda", ... or empty/"auto" (prefer
    // GPU when one is available). Resolved at runtime by ComputeBackends.
    QString computeBackend() const;
    void setComputeBackend(const QString& id);

    // Crash breadcrumb covering the WHOLE model load (CPU and GPU). Loading a
    // whisper model can hard-crash the process in ways C++ can't catch — a GPU
    // GGML_ABORT, or on the CPU path an OOM that segfaults on a null tensor
    // buffer / trips WHISPER_ASSERT. AppController writes this (model path +
    // whether GPU was used) right before whisper init and clears it on success.
    // If it's still set next launch, that exact load killed the app, so a
    // crashed GPU load is retried on CPU. Writes are flushed immediately; no
    // changed().
    QString loadAttemptPath() const;
    bool loadAttemptWasGpu() const;
    void setLoadAttempt(const QString& modelPath, bool gpu);
    void clearLoadAttempt();

    // --- Hotkey ----------------------------------------------------------
    QString hotkey() const;              // e.g. "Ctrl+Alt+Space"
    void setHotkey(const QString& seq);

    QString translateHotkey() const;     // toggle translate-to-English
    void setTranslateHotkey(const QString& seq);

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

    // --- Diagnostics -----------------------------------------------------
    // Write a diagnostic log file (<configDir>/voicetyper.log). On by default
    // so that otherwise-silent crashes leave a trace.
    bool loggingEnabled() const;
    void setLoggingEnabled(bool on);

    // --- Commands config file -------------------------------------------
    QString configDir() const;
    QString commandsConfigPath() const;

    // Reads the commands JSON, materializing the bundled default on first run.
    QString loadCommandsJson() const;
    // Writes the commands JSON to disk. Returns false and sets *error on failure.
    bool saveCommandsJson(const QString& json, QString* error = nullptr);

    // --- Initial prompts file (prompts.json) -----------------------------
    QString promptsConfigPath() const;

    // Whisper initial prompt for a transcription in `language` (see
    // InitialPrompts). Re-reads prompts.json on every call so edits apply to
    // the next dictation; the bundled default is materialized on first run.
    QString initialPrompt(const QString& language, bool translate) const;

    // Bundled Silero VAD model, or empty when the install lacks it.
    static QString vadModelPath();

    // Best-effort scan for a bundled model if none is configured.
    static QString autodetectModelPath();

signals:
    void changed();
};

} // namespace vt
