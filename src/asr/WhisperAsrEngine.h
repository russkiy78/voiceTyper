#pragma once

#include "asr/IAsrEngine.h"

#include <mutex>
#include <string>

struct whisper_context; // fwd-decl from whisper.cpp

namespace vt {

// Local ASR backend backed by whisper.cpp. One model is loaded for the lifetime
// of the engine. transcribe() is serialized with an internal mutex so the final
// pass and the command-detection loop can share a single context safely.
class WhisperAsrEngine final : public IAsrEngine {
public:
    // useGpu/gpuDevice map directly to whisper_context_params; backendLabel is a
    // human-readable description of the selected compute backend for logging.
    WhisperAsrEngine(const std::string& modelPath, bool useGpu, int gpuDevice,
                     std::string backendLabel = {});
    ~WhisperAsrEngine() override;

    WhisperAsrEngine(const WhisperAsrEngine&) = delete;
    WhisperAsrEngine& operator=(const WhisperAsrEngine&) = delete;

    TranscriptionResult transcribe(const AudioBuffer& audio,
                                   const TranscriptionOptions& options) override;

    [[nodiscard]] bool isReady() const override;
    [[nodiscard]] std::string backendName() const override;

private:
    whisper_context* ctx_ = nullptr;
    std::string modelPath_;
    std::string backendLabel_;
    mutable std::mutex mutex_;
};

} // namespace vt
