#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace vt {

struct TranscriptionOptions {
    std::string language = "auto"; // ISO code ("ru", "en", ...) or "auto"
    bool translate = false;        // translate to English instead of transcribe
    int threads = 0;               // 0 => engine picks a sensible default
    // Text whisper treats as preceding the audio (see prompts.json); steers
    // punctuation and casing. Empty => none. Output that merely repeats it is
    // discarded.
    std::string initialPrompt;
    // Decode only the speech the engine's VAD detects (when it has one).
    // false => the whole recording is decoded, silence included.
    bool useVad = true;
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
