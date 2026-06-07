#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>

namespace vt {

class AudioRecorder;
class IAsrEngine;
class CommandEngine;

// While recording, periodically transcribes the most recent audio tail with a
// fast ASR pass and checks it for a stop_recording command. On a hit it emits
// stopDetected() (queued onto the owning thread) so recording can be ended by
// voice without a second hotkey press.
//
// Each pass runs on a worker thread; passes never overlap (guarded by inFlight_)
// and are joined on stop()/destruction, so the borrowed engine/recorder pointers
// are never used past their lifetime.
class CommandDetectionLoop : public QObject {
    Q_OBJECT
public:
    CommandDetectionLoop(AudioRecorder* recorder, IAsrEngine* asr,
                         const CommandEngine* commands, QObject* parent = nullptr);
    ~CommandDetectionLoop() override;

    void setLanguage(const QString& code) { language_ = code; }
    void setIntervalMs(int ms) { intervalMs_ = ms; }
    void setWindowSeconds(double s) { windowSeconds_ = s; }

    void start();
    void stop();

signals:
    void stopDetected();

private slots:
    void tick();
    void onStopDetectedInternal();

private:
    AudioRecorder* recorder_ = nullptr;
    IAsrEngine* asr_ = nullptr;
    const CommandEngine* commands_ = nullptr;

    QTimer timer_;
    std::thread worker_;
    std::atomic<bool> inFlight_{false};
    std::atomic<bool> active_{false};
    // Cancels the in-flight pass so stop() doesn't block on a slow inference.
    // Touched only on the owning thread; the worker reads the pointee.
    std::shared_ptr<std::atomic<bool>> abortFlag_;

    QString language_ = QStringLiteral("auto");
    int intervalMs_ = 2000;
    double windowSeconds_ = 4.0;
};

} // namespace vt
