#pragma once

#include <cstdint>
#include <vector>

namespace vt {

// Canonical internal audio representation expected by the ASR engine:
// mono, 16 kHz, 32-bit float PCM normalized to [-1.0, 1.0].
struct AudioBuffer {
    static constexpr int kTargetSampleRate = 16000;

    std::vector<float> samples;          // mono
    int sampleRate = kTargetSampleRate;

    [[nodiscard]] bool empty() const { return samples.empty(); }
    [[nodiscard]] std::size_t size() const { return samples.size(); }

    [[nodiscard]] double durationSeconds() const {
        return sampleRate > 0
                   ? static_cast<double>(samples.size()) / sampleRate
                   : 0.0;
    }
};

} // namespace vt
