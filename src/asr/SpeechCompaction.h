#pragma once

#include <vector>

namespace vt {

// A stretch of detected speech, in samples: [start, end).
struct SpeechSpan {
    int start = 0;
    int end = 0;
};

// Builds the audio whisper should see from the VAD's speech spans. Silence
// before the first and after the last span is cut down to `edgePad` samples,
// and a pause between spans longer than `maxPause` samples is shortened to
// `maxPause` by dropping its middle. Shorter pauses are kept whole: whisper
// reads them as sentence and clause boundaries, so removing them would cost
// punctuation. Returns an empty buffer when there are no spans.
std::vector<float> compactSpeech(const std::vector<float>& samples,
                                 std::vector<SpeechSpan> spans, int edgePad,
                                 int maxPause);

} // namespace vt
