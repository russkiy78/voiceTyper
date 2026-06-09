#pragma once

#include "commands/CommandEngine.h"

#include <QObject>
#include <QString>

#include <memory>
#include <thread>

namespace vt {

class SettingsStore;
class IAsrEngine;
class ITextPostProcessor;
class RecordingController;
class ClipboardPasteService;
class HotkeyService;
class OverlayWindow;
class TrayController;
class SettingsWindow;

// Top-level coordinator. Wires the tray, global hotkey, recording, ASR,
// command processing and clipboard paste into the end-to-end dictation flow:
//
//   startRecording -> (live stop detection) -> stopRecording
//     -> transcribe (worker thread) -> processCommands -> postProcess
//     -> pasteText (clipboard + synthesized paste + restore)
class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool initialize();

public slots:
    void toggleRecording();
    void openSettings();
    void quit();

private slots:
    void onRecordingStarted();
    void onRecordingStopped(bool stoppedByVoice);
    void onRecordingFailed(const QString& message);
    void onLevel(double level);
    void onDuration(double seconds);
    void onSettingsApplied();

private:
    void buildAsrEngine();
    // Shows the recovery note set by buildAsrEngine() (GPU->CPU fallback, or a
    // quarantined model) once a tray exists — buildAsrEngine runs before the
    // tray during initialize().
    void flushPendingNotice();
    void rebuildRecording();
    void wireRecordingController();
    void buildPostProcessor();
    void applyHotkey();
    void reloadCommands();
    void startTranscription();
    void finishTranscription(const QString& rawText);

    std::unique_ptr<IAsrEngine> asr_;
    CommandEngine commandEngine_;
    std::unique_ptr<ITextPostProcessor> postProcessor_;

    SettingsStore* settings_ = nullptr;
    RecordingController* recording_ = nullptr;
    ClipboardPasteService* paste_ = nullptr;
    HotkeyService* hotkey_ = nullptr;
    OverlayWindow* overlay_ = nullptr;
    TrayController* tray_ = nullptr;
    SettingsWindow* settingsWindow_ = nullptr;

    std::thread worker_;
    QString lastModelPath_;
    QString lastComputeBackend_;

    // Set by buildAsrEngine() when a recovery happened (GPU disabled after a
    // failure, or a model quarantined after it crashed the loader); shown via
    // the tray when one is available, then cleared.
    QString pendingNotice_;

    bool processing_ = false;
};

} // namespace vt
