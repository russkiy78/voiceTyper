// Integration test: requires a model and the upstream samples/jfk.mp3 decoded
// to mono 16 kHz float32 PCM. Link WhisperAsrEngine, ComputeBackends and their
// Qt/whisper dependencies. Usage: test MODEL JFK.f32 [cuda|cpu]
#include "asr/WhisperAsrEngine.h"
#include "asr/ComputeBackends.h"
#include "core/Logging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

Q_LOGGING_CATEGORY(vtAsr, "voicetyper.asr")

int main(int argc, char** argv) {
    if (argc < 3)
        return 1;
    const auto backend = vt::resolveBackend(argc > 3 ? argv[3] : "cpu");
    if (argc > 3 && std::strcmp(argv[3], "cuda") == 0 && !backend.useGpu)
        return 2;
    vt::WhisperAsrEngine engine(argv[1], backend.useGpu, backend.gpuDevice,
                               false, backend.label);
    if (!engine.isReady() || engine.gpuInitFailed())
        return 3;
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 11 * 16000 * sizeof(float) || bytes.size() % sizeof(float))
        return 4;
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), bytes.size());

    for (const char* language : {"auto", "en"}) {
        for (double seconds : {2.5, 5.0, 11.0}) {
            vt::AudioBuffer audio;
            audio.samples.assign(samples.begin(), samples.begin() +
                                 static_cast<std::size_t>(seconds * 16000));
            audio.samples.resize(audio.size() + 5 * 16000, 0.0f);
            vt::TranscriptionOptions options;
            options.language = language;
            options.threads = 4;
            // A live command pass precedes the final pass on the same context.
            options.fastMode = true;
            if (!engine.transcribe(audio, options).ok)
                return 5;
            options.fastMode = false;
            const auto result = engine.transcribe(audio, options);
            const std::string phrase = "my fellow Americans";
            const auto first = result.text.find(phrase);
            if (!result.ok || first == std::string::npos ||
                result.text.find(phrase, first + phrase.size()) != std::string::npos)
                return 6;
            if (seconds == 11 && result.text.find("your country") == std::string::npos)
                return 7;
        }
    }
    vt::AudioBuffer silence;
    silence.samples.resize(5 * 16000);
    vt::TranscriptionOptions options;
    options.language = "ru";
    for (float floor : {0.0f, 1.0e-7f}) {
        std::fill(silence.samples.begin(), silence.samples.end(), floor);
        const auto result = engine.transcribe(silence, options);
        if (!result.ok || !result.text.empty())
            return 8;
    }
    std::puts("PASS: short speech, trailing pauses, shared context and silence");
}
