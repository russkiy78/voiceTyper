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
#include "postprocess/HttpTextPostProcessor.h"
#include "postprocess/NoOpTextPostProcessor.h"
#include "settings/SettingsStore.h"
#include "ui/OverlayWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/ToastOverlay.h"
#include "ui/TrayController.h"

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
    buildPostProcessor();

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
    connect(tray_, &TrayController::openSettingsRequested, this,
            &AppController::openSettings);
    connect(tray_, &TrayController::quitRequested, this, &AppController::quit);

    hotkey_ = HotkeyService::create(this);
    connect(hotkey_, &HotkeyService::activated, this,
            &AppController::toggleRecording);
    connect(hotkey_, &HotkeyService::registrationFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "Hotkey registration failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper — hotkey"), reason);
            });

    translateHotkey_ = HotkeyService::create(this);
    connect(translateHotkey_, &HotkeyService::activated, this,
            &AppController::toggleTranslate);
    connect(translateHotkey_, &HotkeyService::registrationFailed, this,
            [this](const QString& reason) {
                qCWarning(vtApp) << "Translation hotkey registration failed:" << reason;
                if (tray_)
                    tray_->showMessage(tr("voiceTyper — translation hotkey"), reason);
            });

    rebuildRecording();

    applyHotkey();
    applyTranslateHotkey();

    if (!tray_->isAvailable())
        qCWarning(vtApp) << "System tray not available on this platform";
    tray_->setTranslate(settings_->translate());
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
        lastModelPath_.toStdString(), rb.useGpu, rb.gpuDevice, false, rb.label);

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

void AppController::buildPostProcessor() {
    if (settings_->postProcessEnabled() &&
        !settings_->postProcessEndpoint().isEmpty()) {
        postProcessor_ = std::make_unique<HttpTextPostProcessor>(
            settings_->postProcessEndpoint().toStdString());
    } else {
        postProcessor_ = std::make_unique<NoOpTextPostProcessor>();
    }
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

void AppController::toggleTranslate() {
    const bool on = !settings_->translate();
    settings_->setTranslate(on);
    if (tray_)
        tray_->setTranslate(on);
    if (toast_)
        toast_->showToast(on ? tr("Translation to English: on")
                             : tr("Translation to English: off"));
}

void AppController::toggleRecording() {
    if (!recording_)
        return;
    if (recording_->isRecording()) {
        recording_->stopRecording();
    } else if (processing_) {
        if (tray_)
            tray_->showMessage(tr("voiceTyper"),
                               tr("Still transcribing the previous take..."));
    } else {
        recording_->startRecording();
    }
}

void AppController::onRecordingStarted() {
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
        overlay_->setStatus(tr("Transcribing..."));

    processing_ = true;
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
    processing_ = false;
}

void AppController::onLevel(double level) {
    if (overlay_ && overlay_->isVisible())
        overlay_->setLevel(level);
}

void AppController::onDuration(double seconds) {
    if (overlay_ && overlay_->isVisible())
        overlay_->setElapsedSeconds(seconds);
}

void AppController::startTranscription() {
    AudioBuffer audio = recording_->recorder()->snapshotAll();

    if (!asr_->isReady()) {
        processing_ = false;
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

    if (worker_.joinable())
        worker_.join();

    worker_ = std::thread([this, audio = std::move(audio), opt]() {
        const TranscriptionResult res = asr_->transcribe(audio, opt);
        const QString text =
            res.ok ? QString::fromStdString(res.text) : QString();
        qCInfo(vtAsr) << "Transcribed" << audio.durationSeconds()
                      << "s of audio in" << res.durationSeconds << "s";
        QMetaObject::invokeMethod(
            this, [this, text]() { finishTranscription(text); },
            Qt::QueuedConnection);
    });
}

void AppController::finishTranscription(const QString& rawText) {
    processing_ = false;
    if (overlay_)
        overlay_->hideOverlay();

    qCInfo(vtApp) << "Recognized text:" << rawText;

    const CommandProcessingResult processed =
        commandEngine_.processFinalText(rawText.toStdString());
    const std::string cleaned = postProcessor_->process(processed.text);
    const QString finalText = QString::fromStdString(cleaned);

    if (finalText.isEmpty()) {
        if (!rawText.isEmpty())
            qCInfo(vtApp) << "Final text empty after command processing";
        else if (tray_)
            tray_->showMessage(tr("voiceTyper"), tr("Nothing was recognized."));
        return;
    }

    qCInfo(vtApp) << "Pasting" << finalText.size() << "chars:" << finalText;
    paste_->pasteText(finalText);
}

void AppController::onSettingsApplied() {
    if (tray_)
        tray_->setTranslate(settings_->translate());
    paste_->setRestoreDelayMs(settings_->clipboardRestoreDelayMs());
    reloadCommands();
    buildPostProcessor();
    applyHotkey();
    applyTranslateHotkey();
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
    if (worker_.joinable())
        worker_.join();
    // Clear the first-inference breadcrumb if the user closes before any
    // transcription happened; without this the next launch would see a stale
    // GPU breadcrumb and fall back to CPU unnecessarily.
    settings_->clearLoadAttempt();
    QCoreApplication::quit();
}

} // namespace vt
