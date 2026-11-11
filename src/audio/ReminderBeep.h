#pragma once

#include <QBuffer>
#include <QObject>
#include <QTimer>

#include <memory>

class QAudioSink;

namespace vt {

// Plays a short, quiet beep when started and then every kIntervalSeconds
// while running, as a reminder that dictation is still recording. The tone is
// synthesized for the default output device at each beep, so a device
// switched mid-take is followed.
class ReminderBeep : public QObject {
    Q_OBJECT
public:
    static constexpr int kIntervalSeconds = 5;

    explicit ReminderBeep(QObject* parent = nullptr);
    ~ReminderBeep() override;

    void start(); // first beep right away, then one every interval
    void stop();

private:
    void beep();

    QTimer timer_;
    QBuffer buffer_;                  // the tone the sink reads
    std::unique_ptr<QAudioSink> sink_; // declared after buffer_: destroyed first
};

} // namespace vt
