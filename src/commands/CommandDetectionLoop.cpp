#include "commands/CommandDetectionLoop.h"

#include "asr/IAsrEngine.h"
#include "audio/AudioRecorder.h"
#include "commands/CommandEngine.h"
#include "core/Logging.h"

namespace vt {

CommandDetectionLoop::CommandDetectionLoop(AudioRecorder* recorder,
                                           IAsrEngine* asr,
                                           const CommandEngine* commands,
                                           QObject* parent)
    : QObject(parent), recorder_(recorder), asr_(asr), commands_(commands) {
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &CommandDetectionLoop::tick);
}

CommandDetectionLoop::~CommandDetectionLoop() {
    active_ = false;
    if (worker_.joinable())
        worker_.join();
}

void CommandDetectionLoop::start() {
    if (active_)
        return;
    active_ = true;
    timer_.start(intervalMs_);
    qCDebug(vtCmd) << "Command detection loop started, interval" << intervalMs_
                   << "ms, window" << windowSeconds_ << "s";
}

void CommandDetectionLoop::stop() {
    active_ = false;
    timer_.stop();
    // Ask the in-flight pass (if any) to bail out so the join below returns
    // promptly instead of freezing the caller until whisper finishes.
    if (abortFlag_)
        abortFlag_->store(true, std::memory_order_relaxed);
    if (worker_.joinable())
        worker_.join();
    abortFlag_.reset();
}

void CommandDetectionLoop::tick() {
    if (!active_ || inFlight_.load())
        return;
    if (!recorder_ || !asr_ || !commands_ || !asr_->isReady())
        return;

    AudioBuffer tail = recorder_->snapshotTail(windowSeconds_);
    if (tail.durationSeconds() < 1.0)
        return; // not enough audio yet

    // The previous worker has finished (inFlight_ == false); join before reuse.
    if (worker_.joinable())
        worker_.join();

    inFlight_ = true;
    const QString lang = language_;
    IAsrEngine* asr = asr_;
    const CommandEngine* commands = commands_;
    abortFlag_ = std::make_shared<std::atomic<bool>>(false);
    auto abortFlag = abortFlag_;

    worker_ = std::thread([this, asr, commands, lang, abortFlag,
                           tail = std::move(tail)]() {
        TranscriptionOptions opt;
        opt.language = lang.toStdString();
        opt.fastMode = true;
        opt.abortFlag = abortFlag;
        const TranscriptionResult res = asr->transcribe(tail, opt);
        const bool stop = res.ok && commands->containsStopCommand(res.text);
        if (stop) {
            qCInfo(vtCmd) << "Stop command detected mid-recording:"
                          << QString::fromStdString(res.text);
            QMetaObject::invokeMethod(this, "onStopDetectedInternal",
                                      Qt::QueuedConnection);
        }
        inFlight_ = false;
    });
}

void CommandDetectionLoop::onStopDetectedInternal() {
    if (active_)
        emit stopDetected();
}

} // namespace vt
