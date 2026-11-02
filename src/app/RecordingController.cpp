#include "app/RecordingController.h"

#include "asr/IAsrEngine.h"
#include "audio/AudioRecorder.h"
#include "audio/ReminderBeep.h"
#include "commands/CommandDetectionLoop.h"
#include "core/Logging.h"
#include "settings/SettingsStore.h"

namespace vt {

RecordingController::RecordingController(IAsrEngine* asr,
                                        const CommandEngine* commands,
                                        SettingsStore* settings, QObject* parent)
    : QObject(parent), settings_(settings), asr_(asr) {
    recorder_ = new AudioRecorder(this);
    detection_ = new CommandDetectionLoop(recorder_, asr, commands, this);
    reminder_ = new ReminderBeep(this);

    connect(recorder_, &AudioRecorder::levelChanged, this,
            &RecordingController::levelChanged);
    connect(recorder_, &AudioRecorder::durationChanged, this,
            &RecordingController::durationChanged);
    connect(recorder_, &AudioRecorder::errorOccurred, this,
            &RecordingController::recordingFailed);
    connect(detection_, &CommandDetectionLoop::stopDetected, this,
            &RecordingController::stopRecordingByVoiceCommand);
}

void RecordingController::startRecording() {
    if (recording_)
        return;

    if (!recorder_->start()) {
        // errorOccurred() already forwarded as recordingFailed.
        return;
    }
    recording_ = true;

    if (settings_ && settings_->commandDetectionEnabled() && asr_ &&
        asr_->isReady()) {
        detection_->setLanguage(settings_->language());
        detection_->setUseVad(settings_->vadEnabled());
        detection_->setIntervalMs(settings_->commandDetectionIntervalMs());
        detection_->setWindowSeconds(settings_->commandDetectionWindowSeconds());
        detection_->start();
    }

    if (settings_ && settings_->reminderBeepEnabled())
        reminder_->start();

    qCInfo(vtApp) << "Recording started";
    emit recordingStarted();
}

void RecordingController::stopRecording() { stopInternal(false); }

void RecordingController::stopRecordingByVoiceCommand() { stopInternal(true); }

void RecordingController::cancelRecording() {
    if (endCapture())
        qCInfo(vtApp) << "Recording cancelled";
}

void RecordingController::stopInternal(bool byVoice) {
    if (!endCapture())
        return;
    qCInfo(vtApp) << "Recording stopped" << (byVoice ? "(voice)" : "(manual)");
    emit recordingStopped(byVoice);
}

bool RecordingController::endCapture() {
    if (!recording_)
        return false;
    recording_ = false;

    reminder_->stop();
    detection_->stop();
    recorder_->stop();
    return true;
}

} // namespace vt
