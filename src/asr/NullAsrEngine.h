#pragma once

#include "asr/IAsrEngine.h"

namespace vt {

// Fallback engine used when whisper.cpp is compiled out or no model is loaded.
// Always returns an empty transcription so the rest of the pipeline still runs.
class NullAsrEngine final : public IAsrEngine {
public:
    TranscriptionResult transcribe(const AudioBuffer&,
                                   const TranscriptionOptions&) override {
        TranscriptionResult r;
        r.ok = false;
        r.text.clear();
        return r;
    }

    [[nodiscard]] bool isReady() const override { return false; }
    [[nodiscard]] std::string backendName() const override { return "disabled"; }
};

} // namespace vt
