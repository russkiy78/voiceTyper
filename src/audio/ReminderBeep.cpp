#include "audio/ReminderBeep.h"

#include "core/Logging.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace vt {

namespace {

constexpr double kToneHz = 880.0;
constexpr double kToneSeconds = 0.08;
constexpr double kFadeSeconds = 0.01; // raised-cosine edges: no clicks
// About -40 dBFS: a faint tick, meant for a quiet room rather than over
// music or noise.
constexpr double kAmplitude = 0.01;
// Silence after the tone. Should a backend drop still-queued audio when the
// sink is stopped at IdleState, it cuts this instead of the tone.
constexpr double kTailSeconds = 0.1;

void writeSample(char* p, float v, QAudioFormat::SampleFormat fmt) {
    switch (fmt) {
    case QAudioFormat::UInt8: {
        const auto s = static_cast<quint8>(std::lround(v * 127.0f) + 128);
        std::memcpy(p, &s, sizeof(s));
        break;
    }
    case QAudioFormat::Int16: {
        const auto s = static_cast<qint16>(std::lround(v * 32767.0f));
        std::memcpy(p, &s, sizeof(s));
        break;
    }
    case QAudioFormat::Int32: {
        const auto s = static_cast<qint32>(std::lround(v * 2147483647.0));
        std::memcpy(p, &s, sizeof(s));
        break;
    }
    case QAudioFormat::Float:
        std::memcpy(p, &v, sizeof(v));
        break;
    default:
        break;
    }
}

// The tone plus its silent tail as interleaved PCM in `format`; empty when the
// format is unusable.
QByteArray synthesizeTone(const QAudioFormat& format) {
    const int rate = format.sampleRate();
    const int channels = format.channelCount();
    const int bytesPerSample = format.bytesPerSample();
    if (rate <= 0 || channels <= 0 || bytesPerSample <= 0)
        return {};

    const auto toneFrames = static_cast<int>(kToneSeconds * rate);
    const auto fadeFrames =
        std::max(1, static_cast<int>(kFadeSeconds * rate));
    const auto totalFrames =
        toneFrames + static_cast<int>(kTailSeconds * rate);
    const int frameBytes = bytesPerSample * channels;

    QByteArray pcm(static_cast<qsizetype>(totalFrames) * frameBytes, '\0');
    char* out = pcm.data();
    for (int i = 0; i < totalFrames; ++i) {
        float v = 0.0f;
        if (i < toneFrames) {
            const int edge = std::min(i, toneFrames - 1 - i);
            const double gain =
                edge >= fadeFrames
                    ? 1.0
                    : 0.5 - 0.5 * std::cos(std::numbers::pi * edge / fadeFrames);
            v = static_cast<float>(
                kAmplitude * gain *
                std::sin(2.0 * std::numbers::pi * kToneHz * i / rate));
        }
        for (int c = 0; c < channels; ++c)
            writeSample(out + (static_cast<qsizetype>(i) * channels + c) *
                                  bytesPerSample,
                        v, format.sampleFormat());
    }
    return pcm;
}

} // namespace

ReminderBeep::ReminderBeep(QObject* parent) : QObject(parent) {
    timer_.setInterval(kIntervalSeconds * 1000);
    connect(&timer_, &QTimer::timeout, this, &ReminderBeep::beep);
}

ReminderBeep::~ReminderBeep() { stop(); }

void ReminderBeep::start() {
    beep();
    timer_.start();
}

void ReminderBeep::stop() {
    timer_.stop();
    sink_.reset();
    buffer_.close();
}

void ReminderBeep::beep() {
    // The previous beep, if any, finished seconds ago; its sink is idle.
    sink_.reset();
    buffer_.close();

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return;
    const QAudioFormat format = device.preferredFormat();
    const QByteArray pcm = synthesizeTone(format);
    if (pcm.isEmpty())
        return;

    buffer_.setData(pcm);
    buffer_.open(QIODevice::ReadOnly);
    sink_ = std::make_unique<QAudioSink>(device, format);
    QAudioSink* sink = sink_.get();
    connect(sink, &QAudioSink::stateChanged, this,
            [sink](QAudio::State state) {
                // Out of data: release the output stream until the next beep.
                if (state == QAudio::IdleState)
                    sink->stop();
            });
    sink->start(&buffer_);
    if (sink->error() != QAudio::NoError)
        qCWarning(vtAudio) << "Reminder beep failed to play:" << sink->error();
}

} // namespace vt
