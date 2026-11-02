#include "app/AppController.h"

#include "app/RecordingController.h"
#include "asr/IAsrEngine.h"
#include "asr/NullAsrEngine.h"
#include "audio/AudioRecorder.h"
#include "clipboard/ClipboardPasteService.h"
#include "commands/CommandConfig.h"
#include "core/FileLogging.h"
#include "core/Logging.h"
#include "hotkey/HotkeyService.h"
#include "postprocess/LlmPostProcessor.h"
#include "settings/SettingsStore.h"
#include "ui/OverlayWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/ToastOverlay.h"
#include "ui/TrayController.h"

#ifdef Q_OS_MAC
#include "ui/MacWindowUtils.h"
#endif

#include "asr/ComputeBackends.h"

#ifdef VOICETYPER_WITH_WHISPER
#include "asr/WhisperAsrEngine.h"
#endif

#include <QCoreApplication>
#include <QtGlobal>

namespace vt {

AppController::AppController(QObject* parent) : QObject(parent) {}

AppController::~AppController() {
    if (worker_.joinable())
        worker_.join();
    delete overlay_;
    delete toast_;
    delete settingsWindow_;
}

bool AppController::initialize() {
    settings_ = new SettingsStore(this);

    buildAsrEngine();
    reloadCommands();
    llm_ = new LlmPostProcessor(this);

    paste_ = new ClipboardPasteService(this);
    paste_->setRestoreDelayMs(settings_->clipboardRestoreDelayMs());
    connect(paste_, &ClipboardPasteService::pasteFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "Paste failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper"), reason);
            });

    overlay_ = new OverlayWindow();
    toast_   = new ToastOverlay();

    tray_ = new TrayController(this);
    connect(tray_, &TrayController::toggleRecordingRequested, this,
            &AppController::toggleRecording);
    connect(tray_, &TrayController::cancelRequested, this,
            &AppController::cancelDictation);
    connect(tray_, &TrayController::openSettingsRequested, this,
            &AppController::openSettings);
    connect(tray_, &TrayController::quitRequested, this, &AppController::quit);

    hotkey_ = HotkeyService::create(this);
    hotkey_->setShortcutInfo(QStringLiteral("dictation"), tr("Start or stop dictation"));
    connect(hotkey_, &HotkeyService::activated, this,
            &AppController::toggleRecording);
    connect(hotkey_, &HotkeyService::registrationFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "Hotkey registration failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper — hotkey"), reason);
            });

    translateHotkey_ = HotkeyService::create(this);
    translateHotkey_->setShortcutInfo(QStringLiteral("translate"),
                                      tr("Toggle translation to English"));
    connect(translateHotkey_, &HotkeyService::activated, this,
            &AppController::toggleTranslate);
    connect(translateHotkey_, &HotkeyService::registrationFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "Translation hotkey registration failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper — translation hotkey"), reason);
            });

    llmHotkey_ = HotkeyService::create(this);
    llmHotkey_->setShortcutInfo(QStringLiteral("llm"),
                                tr("Toggle LLM processing"));
    connect(llmHotkey_, &HotkeyService::activated, this,
            &AppController::toggleLlm);
    connect(llmHotkey_, &HotkeyService::registrationFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "LLM hotkey registration failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper — LLM hotkey"), reason);
            });

    rebuildRecording();

    applyHotkey();
    applyTranslateHotkey();
    applyLlmHotkey();

    if (!tray_->isAvailable())
        qCWarning(vtApp) << "System tray not available on this platform";
    tray_->setTranslate(settings_->translate());
    tray_->setLlm(settings_->llmEnabled());
    tray_->show();

    // buildAsrEngine() ran before the tray existed; surface any recovery note
    // (GPU->CPU fallback) now that we can show it.
    flushPendingNotice();

    if (!asr_->isReady()) {
        tray_->showMessage(
            tr("voiceTyper"),
            tr("No speech model loaded. Open Settings and choose a whisper "
               "model (.bin)."));
    }

    qCInfo(vtApp) << "voiceTyper initialized. ASR backend:"
                  << QString::fromStdString(asr_->backendName());
    return true;
}

void AppController::buildAsrEngine() {
    lastModelPath_ = settings_->modelPath();
    lastComputeBackend_ = settings_->computeBackend();

#ifdef VOICETYPER_WITH_WHISPER
    // Capture ggml/whisper diagnostics (incl. the CUDA "error: ..." line and the
    // abort message) into the file log before any model load or inference runs;
    // otherwise a GPU compute crash leaves no cause on disk. Idempotent.
    WhisperAsrEngine::installDiagnostics();

    if (lastModelPath_.isEmpty()) {
        asr_ = std::make_unique<NullAsrEngine>();
        flushPendingNotice();
        return;
    }

    ResolvedBackend rb = resolveBackend(lastComputeBackend_.toStdString());

    // (A) Crash-safe GPU load + first inference. A GPU model load — or the very
    // first whisper_full() call on that GPU context — can hard-crash the process
    // in a way C++ can't catch (a CUDA GGML_ABORT, or a driver/device fault).
    // We leave a breadcrumb on disk before the load and clear it only after the
    // first successful inference returns (see re-arm block below). A GPU
    // breadcrumb still set next launch means something in that window killed the
    // app: retry on CPU, which may run the model even when the GPU can't.
    //
    // Honour that ONLY on the first build of the process. buildAsrEngine() also
    // runs from onSettingsApplied() when the model/backend changes — in-process,
    // with the app demonstrably alive. There a still-armed breadcrumb is our own
    // first-inference guard (GPU loaded fine, user just hasn't dictated yet), NOT
    // a crash; treating it as one silently forced a healthy GPU down to CPU and
    // overwrote the user's backend choice on the next settings-apply.
    const bool freshProcess = !asrEngineBuiltOnce_;
    asrEngineBuiltOnce_ = true;

    if (freshProcess && settings_->loadAttemptPath() == lastModelPath_ &&
        settings_->loadAttemptWasGpu()) {
        settings_->clearLoadAttempt();
        qCWarning(vtAsr) << "Previous GPU load or first inference of"
                         << lastModelPath_ << "crashed; forcing CPU.";
        settings_->setComputeBackend(QStringLiteral("cpu"));
        lastComputeBackend_ = QStringLiteral("cpu");
        rb = resolveBackend("cpu");
        pendingNotice_ =
            tr("GPU acceleration crashed (during load or first inference), so "
               "voiceTyper switched to CPU. Re-enable it in Settings to try "
               "again.");
    } else if (!settings_->loadAttemptPath().isEmpty()) {
        // Not a crash to act on — a stale breadcrumb: our own first-inference
        // guard on an in-process rebuild, a just-retried CPU load, or a leftover
        // from a different model. Clear it and load the requested backend.
        settings_->clearLoadAttempt();
    }

    // Breadcrumb written (and flushed) right before the possibly-fatal load,
    // then cleared once we return from it alive.
    settings_->setLoadAttempt(lastModelPath_, rb.useGpu);

    auto engine = std::make_unique<WhisperAsrEngine>(
        lastModelPath_.toStdString(), rb.useGpu, rb.gpuDevice, false, rb.label,
        SettingsStore::vadModelPath().toStdString());

    settings_->clearLoadAttempt();

    // Re-arm the breadcrumb to cover the first whisper_full() call. CUDA (and
    // sometimes Vulkan) can GGML_ABORT() during inference, not only during init —
    // the log cuts off right at the first "transcribe:" debug line. The callback
    // fires from inside the transcribe() mutex after whisper_full() returns for
    // the first time (regardless of its result code: a non-fatal rc means the GPU
    // survived). quit() also clears it in case the user closes without ever
    // transcribing, so the breadcrumb never causes a false CPU fallback.
    if (rb.useGpu && engine->isReady() && !engine->gpuInitFailed()) {
        settings_->setLoadAttempt(lastModelPath_, /*gpu=*/true);
        engine->setOnFirstGpuInferenceDone([this]() {
            // transcribe() runs on a worker thread; hop to main thread to write
            // QSettings (not thread-safe from workers).
            QMetaObject::invokeMethod(this, [this]() {
                settings_->clearLoadAttempt();
            }, Qt::QueuedConnection);
        });
    }

    // (B) The engine catches *recoverable* GPU init failures and falls back to
    // CPU in-process. If that happened, persist CPU too so we stop re-probing a
    // GPU that can't run the model on every launch, and tell the user.
    if (engine->gpuInitFailed()) {
        settings_->setComputeBackend(QStringLiteral("cpu"));
        lastComputeBackend_ = QStringLiteral("cpu");
        if (pendingNotice_.isEmpty())
            pendingNotice_ =
                tr("GPU acceleration couldn't start, so voiceTyper is using "
                   "CPU. Re-enable it in Settings to try again.");
    }

    if (!engine->isReady())
        qCWarning(vtAsr) << "Model failed to load:" << lastModelPath_;

    asr_ = std::move(engine);
#else
    asr_ = std::make_unique<NullAsrEngine>();
#endif

    flushPendingNotice();
}

void AppController::flushPendingNotice() {
    if (pendingNotice_.isEmpty() || !tray_)
        return;
    tray_->showMessage(tr("voiceTyper"), pendingNotice_);
    pendingNotice_.clear();
}

void AppController::rebuildRecording() {
    if (recording_) {
        recording_->deleteLater();
        recording_ = nullptr;
    }
    recording_ =
        new RecordingController(asr_.get(), &commandEngine_, settings_, this);
    wireRecordingController();
}

void AppController::wireRecordingController() {
    connect(recording_, &RecordingController::recordingStarted, this,
            &AppController::onRecordingStarted);
    connect(recording_, &RecordingController::recordingStopped, this,
            &AppController::onRecordingStopped);
    connect(recording_, &RecordingController::recordingFailed, this,
            &AppController::onRecordingFailed);
    connect(recording_, &RecordingController::levelChanged, this,
            &AppController::onLevel);
    connect(recording_, &RecordingController::durationChanged, this,
            &AppController::onDuration);
}

void AppController::reloadCommands() {
    QString error;
    const CommandConfig cfg =
        CommandConfig::fromJson(settings_->loadCommandsJson(), &error);
    if (!error.isEmpty())
        qCWarning(vtCmd) << "Command config error:" << error;
    commandEngine_.setConfig(cfg);
}

void AppController::applyHotkey() {
    if (hotkey_)
        hotkey_->setHotkey(settings_->hotkey());
}

void AppController::applyTranslateHotkey() {
    if (!translateHotkey_)
        return;
    const QString seq = settings_->translateHotkey();
    if (seq.isEmpty())
        translateHotkey_->unregisterHotkey();
    else
        translateHotkey_->setHotkey(seq);
}

void AppController::applyLlmHotkey() {
    if (!llmHotkey_)
        return;
    const QString seq = settings_->llmHotkey();
    if (seq.isEmpty())
        llmHotkey_->unregisterHotkey();
    else
        llmHotkey_->setHotkey(seq);
}

void AppController::toggleTranslate() {
    const bool on = !settings_->translate();
    settings_->setTranslate(on);
    if (tray_)
        tray_->setTranslate(on);
    if (toast_)
        toast_->showToast(on ? tr("Translation to English: on")
                             : tr("Translation to English: off"));
}

void AppController::toggleLlm() {
    const bool on = !settings_->llmEnabled();
    settings_->setLlmEnabled(on);
    if (tray_)
        tray_->setLlm(on);
    if (!toast_)
        return;
    if (!on)
        toast_->showToast(tr("LLM processing: off"));
    else if (settings_->llmEndpoint().trimmed().isEmpty())
        // Dictation still works: the request fails and the text is pasted
        // unchanged. Say why up front.
        toast_->showToast(tr("LLM processing: on — no endpoint URL set"));
    else
        toast_->showToast(tr("LLM processing: on"));
}

LlmRequestConfig AppController::llmConfig() const {
    LlmRequestConfig cfg;
    cfg.endpoint = settings_->llmEndpoint();
    cfg.apiKey = settings_->llmApiKey();
    cfg.model = settings_->llmModel();
    cfg.promptTemplate = settings_->llmPrompt();
    cfg.stripPattern = settings_->llmStripPattern();
    cfg.timeoutMs = settings_->llmTimeoutSeconds() * 1000;
    QString error;
    if (!parseExtraParams(settings_->llmExtraParams(), &cfg.extraParams, &error))
        qCWarning(vtLlm) << "Extra request fields ignored:" << error;
    return cfg;
}

void AppController::toggleRecording() {
    if (!recording_)
        return;
    if (recording_->isRecording()) {
        recording_->stopRecording();
    } else if (processing_) {
        if (tray_)
            tray_->showMessage(tr("voiceTyper"),
                               tr("Still processing the previous take..."));
    } else {
        recording_->startRecording();
    }
}

void AppController::cancelDictation() {
    if (recording_ && recording_->isRecording()) {
        recording_->cancelRecording();
    } else if (processing_) {
        // The LLM request is dropped without calling back; a transcription
        // that still finishes is ignored.
        if (transcriptionAbort_)
            transcriptionAbort_->store(true);
        llm_->cancel();
        qCInfo(vtApp) << "Processing cancelled";
    } else {
        return;
    }

    setProcessing(false);
    if (tray_)
        tray_->setRecording(false);
    if (overlay_)
        overlay_->hideOverlay();
    if (toast_)
        toast_->showToast(tr("Dictation cancelled"));
}

void AppController::onRecordingStarted() {
#ifdef Q_OS_MAC
    // Snapshot whichever app the user was dictating into before showing the
    // overlay below can shift macOS's active-app focus away from it; restored
    // right before the paste keystroke in finishTranscription().
    mac::rememberFrontmostApp();
#endif
    if (tray_)
        tray_->setRecording(true);
    if (settings_->overlayEnabled() && overlay_) {
        overlay_->setStatus(tr("Recording"));
        overlay_->setElapsedSeconds(0);
        overlay_->setLevel(0);
        overlay_->showOverlay();
    }
}

void AppController::onRecordingStopped(bool stoppedByVoice) {
    Q_UNUSED(stoppedByVoice);
    if (tray_)
        tray_->setRecording(false);
    if (overlay_ && overlay_->isVisible())
        overlay_->showProcessing(tr("Transcribing..."));

    setProcessing(true);
    startTranscription();
}

void AppController::onRecordingFailed(const QString& message) {
    qCWarning(vtApp) << "Recording failed:" << message;
    if (tray_)
        tray_->showMessage(tr("voiceTyper — microphone"), message);
    if (tray_)
        tray_->setRecording(false);
    if (overlay_)
        overlay_->hideOverlay();
    setProcessing(false);
}

void AppController::onLevel(double level) {
    if (overlay_ && overlay_->isVisible())
        overlay_->setLevel(level);
}

void AppController::onDuration(double seconds) {
    if (overlay_ && overlay_->isVisible())
        overlay_->setElapsedSeconds(seconds);
}

void AppController::setProcessing(bool on) {
    processing_ = on;
    if (tray_)
        tray_->setProcessing(on);
}

void AppController::startTranscription() {
    AudioBuffer audio = recording_->recorder()->snapshotAll();

    if (!asr_->isReady()) {
        setProcessing(false);
        if (overlay_)
            overlay_->hideOverlay();
        if (tray_)
            tray_->showMessage(
                tr("voiceTyper"),
                tr("No speech model loaded — nothing was transcribed."));
        return;
    }
    if (audio.empty()) {
        finishTranscription(QString());
        return;
    }

    TranscriptionOptions opt;
    opt.language = settings_->language().toStdString();
    opt.translate = settings_->translate();
    opt.threads = settings_->threads();
    opt.useVad = settings_->vadEnabled();
    // Final pass only: the command-detection loop matches bare phrases, and a
    // prompt there would just be one more thing to echo on a silent window.
    opt.initialPrompt = settings_->initialPrompt(settings_->language(),
                                                 opt.translate)
                            .toStdString();
    transcriptionAbort_ = std::make_shared<std::atomic<bool>>(false);
    opt.abortFlag = transcriptionAbort_;

    if (worker_.joinable())
        worker_.join();

    worker_ = std::thread([this, audio = std::move(audio), opt]() {
        const TranscriptionResult res = asr_->transcribe(audio, opt);
        const QString text =
            res.ok ? QString::fromStdString(res.text) : QString();
        qCInfo(vtAsr) << "Transcribed" << audio.durationSeconds()
                      << "s of audio in" << res.durationSeconds << "s";
        QMetaObject::invokeMethod(
            this,
            [this, text, abort = opt.abortFlag]() {
                if (!abort->load())
                    finishTranscription(text);
            },
            Qt::QueuedConnection);
    });
}

void AppController::finishTranscription(const QString& rawText) {
    qCInfo(vtApp) << "Recognized text:" << rawText;

    const CommandProcessingResult processed =
        commandEngine_.processFinalText(rawText.toStdString());
    const QString text = QString::fromStdString(processed.text);

    if (text.isEmpty()) {
        setProcessing(false);
        if (overlay_)
            overlay_->hideOverlay();
        if (!rawText.isEmpty())
            qCInfo(vtApp) << "Final text empty after command processing";
        else if (tray_)
            tray_->showMessage(tr("voiceTyper"), tr("Nothing was recognized."));
        return;
    }

    if (!settings_->llmEnabled()) {
        pasteFinalText(text);
        return;
    }

    // processing_ stays set until the answer arrives, so a new take can't
    // start and paste ahead of this one.
    if (overlay_ && overlay_->isVisible())
        overlay_->showProcessing(tr("Processing with LLM..."));
    llm_->process(llmConfig(), text,
                  [this, text](const QString& answer, const QString& error) {
                      if (error.isEmpty()) {
                          pasteFinalText(answer);
                          return;
                      }
                      // Dictation must not get lost to a network or server
                      // problem: paste what was said instead.
                      if (tray_)
                          tray_->showMessage(
                              tr("voiceTyper — LLM"),
                              tr("LLM processing failed: %1\nThe dictated text "
                                 "was pasted unchanged.")
                                  .arg(error));
                      pasteFinalText(text);
                  });
}

void AppController::pasteFinalText(const QString& finalText) {
    setProcessing(false);
    if (overlay_)
        overlay_->hideOverlay();

#ifdef Q_OS_MAC
    // Reactivate whatever app was frontmost when recording started, in case
    // showing the overlay shifted focus away from it — the paste keystroke
    // below needs that app to actually be frontmost to land correctly.
    mac::restoreFrontmostApp();
#endif

    qCInfo(vtApp) << "Pasting" << finalText.size() << "chars:" << finalText;
    paste_->pasteText(finalText);
}

void AppController::onSettingsApplied() {
    if (tray_) {
        tray_->setTranslate(settings_->translate());
        tray_->setLlm(settings_->llmEnabled());
    }
    paste_->setRestoreDelayMs(settings_->clipboardRestoreDelayMs());
    reloadCommands();
    applyHotkey();
    applyTranslateHotkey();
    applyLlmHotkey();
    setFileLoggingEnabled(settings_->loggingEnabled());

    // Rebuild the ASR backend (and the recorder that borrows it) if the model
    // or the compute backend changed. Settings are only edited while idle, so
    // this is safe.
    if (settings_->modelPath() != lastModelPath_ ||
        settings_->computeBackend() != lastComputeBackend_) {
        buildAsrEngine();
        rebuildRecording();
        qCInfo(vtApp) << "ASR backend reloaded:"
                      << QString::fromStdString(asr_->backendName());
    }
}

void AppController::openSettings() {
    if (!settingsWindow_) {
        settingsWindow_ = new SettingsWindow(settings_);
        connect(settingsWindow_, &SettingsWindow::settingsApplied, this,
                &AppController::onSettingsApplied);
    }
    settingsWindow_->show();
    settingsWindow_->raise();
    settingsWindow_->activateWindow();
}

void AppController::quit() {
    if (recording_ && recording_->isRecording())
        recording_->stopRecording();
    if (hotkey_)
        hotkey_->unregisterHotkey();
    if (translateHotkey_)
        translateHotkey_->unregisterHotkey();
    if (llmHotkey_)
        llmHotkey_->unregisterHotkey();
    if (llm_)
        llm_->cancel();
    if (worker_.joinable())
        worker_.join();
    // Clear the first-inference breadcrumb if the user closes before any
    // transcription happened; without this the next launch would see a stale
    // GPU breadcrumb and fall back to CPU unnecessarily.
    settings_->clearLoadAttempt();
    QCoreApplication::quit();
}

} // namespace vt
