#pragma once

#include "commands/CommandEngine.h"
#include "postprocess/LlmChat.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>

namespace vt {

class SettingsStore;
class IAsrEngine;
class LlmPostProcessor;
class RecordingController;
class ClipboardPasteService;
class HotkeyService;
class OverlayWindow;
class ToastOverlay;
class TrayController;
class SettingsWindow;

// Top-level coordinator. Wires the tray, global hotkey, recording, ASR,
// command processing and clipboard paste into the end-to-end dictation flow:
//
//   startRecording -> (live stop detection) -> stopRecording
//     -> transcribe (worker thread) -> processCommands
//     -> [LLM processing, when on: async HTTP round trip]
//     -> pasteText (clipboard + synthesized paste + restore)
class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool initialize();

public slots:
    void toggleRecording();
    // Drops the take at any stage before the paste: recording, transcription
    // or the LLM request. Nothing is pasted.
    void cancelDictation();
    void openSettings();
    void quit();

private slots:
    void onRecordingStarted();
    void onRecordingStopped(bool stoppedByVoice);
    void onRecordingFailed(const QString& message);
    void onLevel(double level);
    void onDuration(double seconds);
    void onSettingsApplied();
    void toggleTranslate();
    void toggleLlm();

private:
    void buildAsrEngine();
    // Shows the recovery note set by buildAsrEngine() (GPU->CPU fallback) once a
    // tray exists — buildAsrEngine runs before the tray during initialize().
    void flushPendingNotice();
    void rebuildRecording();
    void wireRecordingController();
    void applyHotkey();
    void applyTranslateHotkey();
    void applyLlmHotkey();
    LlmRequestConfig llmConfig() const;
    void reloadCommands();
    void setProcessing(bool on);
    void startTranscription();
    void finishTranscription(const QString& rawText);
    // Ends the take: clears processing_, hides the overlay, pastes the text.
    void pasteFinalText(const QString& finalText);

    std::unique_ptr<IAsrEngine> asr_;
    CommandEngine commandEngine_;

    SettingsStore* settings_ = nullptr;
    RecordingController* recording_ = nullptr;
    ClipboardPasteService* paste_ = nullptr;
    HotkeyService* hotkey_ = nullptr;
    HotkeyService* translateHotkey_ = nullptr;
    HotkeyService* llmHotkey_ = nullptr;
    LlmPostProcessor* llm_ = nullptr;
    OverlayWindow* overlay_ = nullptr;
    ToastOverlay* toast_ = nullptr;
    TrayController* tray_ = nullptr;
    SettingsWindow* settingsWindow_ = nullptr;

    std::thread worker_;
    QString lastModelPath_;
    QString lastComputeBackend_;

    // Set by buildAsrEngine() when a recovery happened (GPU disabled after a
    // failure); shown via the tray when one is available, then cleared.
    QString pendingNotice_;

    // From the end of recording until the paste, LLM wait included.
    bool processing_ = false;

    // Set by cancelDictation(): aborts the take's whisper pass, and its result
    // is dropped. A new flag per take.
    std::shared_ptr<std::atomic<bool>> transcriptionAbort_;

    // buildAsrEngine() runs once at startup and again on every settings-apply
    // that changes the model/backend. The on-disk GPU crash breadcrumb may be
    // read as a crash only on the first (fresh-process) build; on an in-process
    // rebuild it's our own still-armed first-inference guard, not a crash.
    bool asrEngineBuiltOnce_ = false;
};

} // namespace vt
