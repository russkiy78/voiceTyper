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
#include "ui/TrayController.h"

#include "asr/ComputeBackends.h"

#ifdef VOICETYPER_WITH_WHISPER
#include "asr/WhisperAsrEngine.h"
#endif

#include <QCoreApplication>
#include <QFileInfo>
#include <QtGlobal>

namespace vt {

AppController::AppController(QObject* parent) : QObject(parent) {}

AppController::~AppController() {
    if (worker_.joinable())
        worker_.join();
    delete overlay_;
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

    rebuildRecording();

    applyHotkey();

    if (!tray_->isAvailable())
        qCWarning(vtApp) << "System tray not available on this platform";
    tray_->show();

    // buildAsrEngine() ran before the tray existed; surface any recovery note
    // (GPU->CPU fallback or a quarantined model) now that we can show it.
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
    if (lastModelPath_.isEmpty()) {
        asr_ = std::make_unique<NullAsrEngine>();
        flushPendingNotice();
        return;
    }

    // A model previously quarantined for crashing the loader stays disabled
    // until the user explicitly re-picks it (setModelPath lifts the quarantine).
    if (lastModelPath_ == settings_->quarantinedModel()) {
        qCWarning(vtAsr) << "Model is quarantined (crashed the loader before):"
                         << lastModelPath_ << "- not loading.";
        pendingNotice_ =
            tr("voiceTyper disabled \"%1\" because it crashed while loading. "
               "Pick a different model in Settings (try a smaller one).")
                .arg(QFileInfo(lastModelPath_).fileName());
        asr_ = std::make_unique<NullAsrEngine>();
        flushPendingNotice();
        return;
    }

    ResolvedBackend rb = resolveBackend(lastComputeBackend_.toStdString());

    // (A) Crash-safe model load. Loading a model can hard-crash the process in
    // ways C++ can't catch (a GPU GGML_ABORT, or on CPU an OOM that segfaults on
    // a null tensor buffer / trips WHISPER_ASSERT), so we leave a breadcrumb on
    // disk before the load and clear it on success. Finding it still set means
    // the previous load of THIS model killed the app — escalate the recovery:
    //   - a crashed GPU load  -> retry on CPU (the GPU may just be unable to run
    //                            the model; CPU still might);
    //   - a crashed CPU load  -> the model itself is unloadable here, quarantine
    //                            it so we stop crash-looping.
    if (settings_->loadAttemptPath() == lastModelPath_) {
        const bool wasGpu = settings_->loadAttemptWasGpu();
        settings_->clearLoadAttempt();
        if (wasGpu) {
            qCWarning(vtAsr) << "Previous GPU load of" << lastModelPath_
                             << "crashed; forcing CPU.";
            settings_->setComputeBackend(QStringLiteral("cpu"));
            lastComputeBackend_ = QStringLiteral("cpu");
            rb = resolveBackend("cpu");
            pendingNotice_ =
                tr("GPU acceleration crashed last time, so voiceTyper switched "
                   "to CPU. Re-enable it in Settings to try again.");
        } else {
            qCWarning(vtAsr) << "Previous CPU load of" << lastModelPath_
                             << "crashed; quarantining the model.";
            settings_->setQuarantinedModel(lastModelPath_);
            pendingNotice_ =
                tr("voiceTyper disabled \"%1\" because it crashed while loading. "
                   "Pick a different model in Settings (try a smaller one).")
                    .arg(QFileInfo(lastModelPath_).fileName());
            asr_ = std::make_unique<NullAsrEngine>();
            flushPendingNotice();
            return;
        }
    } else if (!settings_->loadAttemptPath().isEmpty()) {
        // Stale breadcrumb from a different model (user changed models since).
        settings_->clearLoadAttempt();
    }

    // Breadcrumb written (and flushed) right before the possibly-fatal load,
    // then cleared once we return from it alive.
    settings_->setLoadAttempt(lastModelPath_, rb.useGpu);

    auto engine = std::make_unique<WhisperAsrEngine>(
        lastModelPath_.toStdString(), rb.useGpu, rb.gpuDevice, false, rb.label);

    settings_->clearLoadAttempt();

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
    paste_->setRestoreDelayMs(settings_->clipboardRestoreDelayMs());
    reloadCommands();
    buildPostProcessor();
    applyHotkey();
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
    if (worker_.joinable())
        worker_.join();
    QCoreApplication::quit();
}

} // namespace vt
