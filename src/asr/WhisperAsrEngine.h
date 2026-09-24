#pragma once

#include "asr/IAsrEngine.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct whisper_context;     // fwd-decl from whisper.cpp
struct whisper_vad_context; // fwd-decl from whisper.cpp

namespace vt {

// Local ASR backend backed by whisper.cpp. One model is loaded for the lifetime
// of the engine. transcribe() is serialized with an internal mutex so the final
// pass and the command-detection loop can share a single context safely.
class WhisperAsrEngine final : public IAsrEngine {
public:
    // useGpu/gpuDevice/flashAttn map directly to whisper_context_params;
    // backendLabel is a human-readable description of the selected compute
    // backend for logging. flashAttn defaults off to preserve pre-1.8 behaviour
    // (upstream flipped its default to on in v1.8.0); enabling it mainly speeds
    // up the GPU encoder but its support/numerics vary by backend, so it is kept
    // an explicit, separately-toggleable knob for A/B benchmarking.
    // vadModelPath is a Silero VAD ggml model (run on CPU); when it loads, only
    // detected speech reaches whisper and audio without speech is not decoded,
    // unless a pass sets TranscriptionOptions::useVad = false.
    // Empty or unloadable => the whole recording is transcribed as before.
    WhisperAsrEngine(const std::string& modelPath, bool useGpu, int gpuDevice,
                     bool flashAttn = false, std::string backendLabel = {},
                     const std::string& vadModelPath = {});
    ~WhisperAsrEngine() override;

    // Route ggml/whisper diagnostics into the app's file-backed Qt log: the
    // detailed "CUDA error: ..." line (ggml_log_set), whisper's own asserts
    // (whisper_log_set), and the final "file:line: ..." printed right before
    // abort() (ggml_set_abort_callback). Without these, ggml writes them to
    // stderr — invisible in a windowed Windows build — so a GPU compute crash
    // leaves the log stopping mid-pass with no cause. Idempotent; call once
    // before loading any model.
    static void installDiagnostics();

    WhisperAsrEngine(const WhisperAsrEngine&) = delete;
    WhisperAsrEngine& operator=(const WhisperAsrEngine&) = delete;

    TranscriptionResult transcribe(const AudioBuffer& audio,
                                   const TranscriptionOptions& options) override;

    [[nodiscard]] bool isReady() const override;
    [[nodiscard]] std::string backendName() const override;

    // True when GPU compute init was requested but failed (threw / returned
    // null) and the engine fell back to CPU in-process. Lets the caller persist
    // CPU so it stops re-probing a GPU that can't run the model every launch.
    [[nodiscard]] bool gpuInitFailed() const { return gpuInitFailed_; }

    // Register a one-shot callback invoked after the first whisper_full() call
    // returns (any result — if it returned, the GPU didn't abort). Called from
    // within the internal mutex, so the callback must not call transcribe().
    // Used by AppController to clear the GPU crash breadcrumb that guards the
    // first inference.
    void setOnFirstGpuInferenceDone(std::function<void()> cb);

private:
    // Speech-only audio for whisper (see compactSpeech). Empty when the VAD
    // found no speech. Caller holds mutex_.
    std::vector<float> extractSpeech(const std::vector<float>& samples,
                                     int sampleRate);

    whisper_context* ctx_ = nullptr;
    whisper_vad_context* vad_ = nullptr;
    std::string modelPath_;
    std::string backendLabel_;
    bool gpuInitFailed_ = false;
    bool firstGpuInferenceDone_ = false;
    std::function<void()> onFirstGpuInferenceDone_;
    mutable std::mutex mutex_;
};

} // namespace vt
