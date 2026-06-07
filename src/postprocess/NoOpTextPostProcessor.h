#pragma once

#include "postprocess/TextPostProcessor.h"

namespace vt {

// Default MVP post-processor: returns the text unchanged.
class NoOpTextPostProcessor final : public ITextPostProcessor {
public:
    std::string process(const std::string& text) override { return text; }
};

} // namespace vt
