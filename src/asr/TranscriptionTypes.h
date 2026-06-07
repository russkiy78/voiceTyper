#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace vt {

struct TranscriptionOptions {
    std::string language = "auto"; // ISO code ("ru", "en", ...) or "auto"
    bool translate = false;        // translate to English instead of transcribe
    int threads = 0;               // 0 => engine picks a sensible default
    // Hint for short, latency-sensitive passes (command detection loop).
    bool fastMode = false;
    // Cooperative cancel. When set and flipped to true mid-flight, the engine
    // aborts the in-progress inference and returns ok == false. Lets a caller
    // stop a slow pass (e.g. the detection loop) without blocking on it.
    std::shared_ptr<std::atomic<bool>> abortFlag;
};

struct TranscriptionResult {
    std::string text;
    double durationSeconds = 0.0;  // wall-clock time spent transcribing
    bool ok = true;                // false when the engine could not run
};

} // namespace vt
