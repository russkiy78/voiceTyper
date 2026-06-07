#include "audio/AudioRecorder.h"

#include "core/Logging.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vt {

namespace {

float sampleToFloat(const char* p, QAudioFormat::SampleFormat fmt) {
    switch (fmt) {
    case QAudioFormat::UInt8: {
        const auto v = static_cast<quint8>(*p);
        return (static_cast<float>(v) - 128.0f) / 128.0f;
    }
    case QAudioFormat::Int16: {
        qint16 v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 32768.0f;
    }
    case QAudioFormat::Int32: {
        qint32 v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 2147483648.0f;
    }
    case QAudioFormat::Float: {
        float v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    default:
        return 0.0f;
    }
}

} // namespace

AudioRecorder::AudioRecorder(QObject* parent) : QObject(parent) {}

AudioRecorder::~AudioRecorder() { stop(); }

bool AudioRecorder::start() {
    if (recording_)
        return true;

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        emit errorOccurred(tr("No audio input device available."));
        return false;
    }

    QAudioFormat want;
    want.setSampleRate(AudioBuffer::kTargetSampleRate);
    want.setChannelCount(1);
    want.setSampleFormat(QAudioFormat::Int16);

    format_ = device.isFormatSupported(want) ? want : device.preferredFormat();
    sourceRate_ = format_.sampleRate();

    {
        QMutexLocker lock(&mutex_);
        mono_.clear();
        mono_.reserve(static_cast<std::size_t>(sourceRate_) * 30); // ~30s headroom
        level_ = 0.0;
    }

    source_ = std::make_unique<QAudioSource>(device, format_);
    io_ = source_->start();
    if (!io_) {
        emit errorOccurred(tr("Failed to start audio capture."));
        source_.reset();
        return false;
    }
    connect(io_, &QIODevice::readyRead, this, &AudioRecorder::onReadyRead);

    recording_ = true;
    qCInfo(vtAudio) << "Recording started:" << format_.sampleRate() << "Hz"
                    << format_.channelCount() << "ch"
                    << static_cast<int>(format_.sampleFormat());
    return true;
}

void AudioRecorder::stop() {
    if (!recording_ && !source_)
        return;
    recording_ = false;
    if (source_) {
        source_->stop();
        source_.reset();
    }
    io_ = nullptr;
    qCInfo(vtAudio) << "Recording stopped, duration" << durationSeconds() << "s";
}

bool AudioRecorder::isRecording() const { return recording_; }

void AudioRecorder::onReadyRead() {
    if (!io_)
        return;
    const QByteArray chunk = io_->readAll();
    if (!chunk.isEmpty())
        appendConverted(chunk.constData(), chunk.size());
}

void AudioRecorder::appendConverted(const char* data, qint64 bytes) {
    const int channels = std::max(1, format_.channelCount());
    const int bytesPerSample = format_.bytesPerSample();
    if (bytesPerSample <= 0)
        return;
    const int frameBytes = bytesPerSample * channels;
    if (frameBytes <= 0)
        return;
    const qint64 frames = bytes / frameBytes;
    const QAudioFormat::SampleFormat fmt = format_.sampleFormat();

    std::vector<float> incoming;
    incoming.reserve(static_cast<std::size_t>(frames));

    double sumSq = 0.0;
    for (qint64 f = 0; f < frames; ++f) {
        const char* frame = data + f * frameBytes;
        float acc = 0.0f;
        for (int c = 0; c < channels; ++c)
            acc += sampleToFloat(frame + c * bytesPerSample, fmt);
        const float mono = acc / static_cast<float>(channels);
        incoming.push_back(mono);
        sumSq += static_cast<double>(mono) * mono;
    }

    if (incoming.empty())
        return;

    const double rms = std::sqrt(sumSq / static_cast<double>(incoming.size()));
    double dur = 0.0;
    {
        QMutexLocker lock(&mutex_);
        mono_.insert(mono_.end(), incoming.begin(), incoming.end());
        level_ = std::clamp(rms * 4.0, 0.0, 1.0); // light gain for the meter
        dur = sourceRate_ > 0
                  ? static_cast<double>(mono_.size()) / sourceRate_
                  : 0.0;
    }

    emit levelChanged(currentLevel());
    emit durationChanged(dur);
}

double AudioRecorder::currentLevel() const {
    QMutexLocker lock(&mutex_);
    return level_;
}

double AudioRecorder::durationSeconds() const {
    QMutexLocker lock(&mutex_);
    return sourceRate_ > 0 ? static_cast<double>(mono_.size()) / sourceRate_ : 0.0;
}

AudioBuffer AudioRecorder::resampleSpanLocked(std::size_t startIndex,
                                              std::size_t count) const {
    // Caller holds mutex_.
    AudioBuffer out;
    out.sampleRate = AudioBuffer::kTargetSampleRate;
    if (count == 0 || sourceRate_ <= 0)
        return out;

    const float* src = mono_.data() + startIndex;

    if (sourceRate_ == AudioBuffer::kTargetSampleRate) {
        out.samples.assign(src, src + count);
        return out;
    }

    const double ratio =
        static_cast<double>(AudioBuffer::kTargetSampleRate) / sourceRate_;
    const auto outN = static_cast<std::size_t>(std::floor(count * ratio));
    out.samples.resize(outN);
    for (std::size_t i = 0; i < outN; ++i) {
        const double srcPos = static_cast<double>(i) / ratio;
        const auto i0 = static_cast<std::size_t>(srcPos);
        const std::size_t i1 = std::min(i0 + 1, count - 1);
        const double frac = srcPos - static_cast<double>(i0);
        out.samples[i] = static_cast<float>((1.0 - frac) * src[i0] +
                                            frac * src[i1]);
    }
    return out;
}

AudioBuffer AudioRecorder::snapshotAll() const {
    QMutexLocker lock(&mutex_);
    return resampleSpanLocked(0, mono_.size());
}

AudioBuffer AudioRecorder::snapshotTail(double seconds) const {
    QMutexLocker lock(&mutex_);
    if (mono_.empty() || seconds <= 0.0)
        return {};
    const auto wanted = static_cast<std::size_t>(seconds * sourceRate_);
    const std::size_t count = std::min(wanted, mono_.size());
    const std::size_t start = mono_.size() - count;
    return resampleSpanLocked(start, count);
}

} // namespace vt
