#pragma once

#include "core/AudioBuffer.h"

#include <QAudioFormat>
#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class QAudioSource;
class QIODevice;

namespace vt {

// Captures microphone audio via Qt Multimedia and exposes it as 16 kHz mono
// float PCM (the format whisper.cpp expects).
//
// The device may not natively support 16 kHz/mono, so capture happens at the
// device's chosen format and is down-mixed + resampled on demand in the
// snapshot* methods. Those methods are mutex-guarded and safe to call from
// worker threads (used by the command-detection loop and final transcription).
class AudioRecorder : public QObject {
    Q_OBJECT
public:
    explicit AudioRecorder(QObject* parent = nullptr);
    ~AudioRecorder() override;

    bool start();
    void stop();
    [[nodiscard]] bool isRecording() const;

    // Entire capture so far, resampled to 16 kHz mono.
    [[nodiscard]] AudioBuffer snapshotAll() const;
    // Last `seconds` of capture, resampled to 16 kHz mono.
    [[nodiscard]] AudioBuffer snapshotTail(double seconds) const;

    [[nodiscard]] double durationSeconds() const;
    [[nodiscard]] double currentLevel() const; // 0..1, recent RMS

signals:
    void levelChanged(double level);
    void durationChanged(double seconds);
    void errorOccurred(const QString& message);

private slots:
    void onReadyRead();

private:
    void appendConverted(const char* data, qint64 bytes);
    [[nodiscard]] AudioBuffer resampleSpanLocked(std::size_t startIndex,
                                                 std::size_t count) const;

    std::unique_ptr<QAudioSource> source_;
    QIODevice* io_ = nullptr; // owned by source_
    QAudioFormat format_;
    int sourceRate_ = AudioBuffer::kTargetSampleRate;

    mutable QMutex mutex_;
    std::vector<float> mono_; // captured mono samples at sourceRate_

    double level_ = 0.0;
    bool recording_ = false;
};

} // namespace vt
