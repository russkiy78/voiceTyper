#pragma once

#include "asr/TranscriptionTypes.h"
#include "core/AudioBuffer.h"

namespace vt {

// Local speech-to-text backend. Implementations must be safe to call from a
// worker thread. The WhisperAsrEngine serializes concurrent calls internally.
class IAsrEngine {
public:
    virtual ~IAsrEngine() = default;

    virtual TranscriptionResult transcribe(const AudioBuffer& audio,
                                           const TranscriptionOptions& options) = 0;

    // True when the backend is loaded and ready to transcribe.
    [[nodiscard]] virtual bool isReady() const = 0;

    // Human-readable backend description (model name / "disabled").
    [[nodiscard]] virtual std::string backendName() const = 0;
};

} // namespace vt
