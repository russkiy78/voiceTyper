#pragma once

#include <QObject>
#include <QString>

namespace vt {

class AudioRecorder;
class CommandDetectionLoop;
class CommandEngine;
class IAsrEngine;
class SettingsStore;

// Owns microphone capture and the live stop-command detection loop, and exposes
// a simple recording state machine. The actual transcription/paste pipeline is
// driven by AppController in response to recordingStopped().
class RecordingController : public QObject {
    Q_OBJECT
public:
    RecordingController(IAsrEngine* asr, const CommandEngine* commands,
                        SettingsStore* settings, QObject* parent = nullptr);

    [[nodiscard]] bool isRecording() const { return recording_; }
    [[nodiscard]] AudioRecorder* recorder() const { return recorder_; }

public slots:
    void startRecording();
    void stopRecording();               // explicit stop (hotkey / tray)
    void stopRecordingByVoiceCommand(); // from the detection loop

signals:
    void recordingStarted();
    void recordingStopped(bool stoppedByVoice);
    void recordingFailed(const QString& message);
    void levelChanged(double level);
    void durationChanged(double seconds);

private:
    void stopInternal(bool byVoice);

    AudioRecorder* recorder_ = nullptr;
    CommandDetectionLoop* detection_ = nullptr;
    SettingsStore* settings_ = nullptr;
    IAsrEngine* asr_ = nullptr;
    bool recording_ = false;
};

} // namespace vt
