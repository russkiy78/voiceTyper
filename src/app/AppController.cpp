#include "app/AppController.h"

#include "app/RecordingController.h"
#include "asr/IAsrEngine.h"
#include "asr/NullAsrEngine.h"
#include "audio/AudioRecorder.h"
#include "clipboard/ClipboardPasteService.h"
#include "commands/CommandConfig.h"
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
    if (!lastModelPath_.isEmpty()) {
        const ResolvedBackend rb =
            resolveBackend(lastComputeBackend_.toStdString());
        asr_ = std::make_unique<WhisperAsrEngine>(
            lastModelPath_.toStdString(), rb.useGpu, rb.gpuDevice, false,
            rb.label);
        if (!asr_->isReady())
            qCWarning(vtAsr) << "Model failed to load:" << lastModelPath_;
    } else {
        asr_ = std::make_unique<NullAsrEngine>();
    }
#else
    asr_ = std::make_unique<NullAsrEngine>();
#endif
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
