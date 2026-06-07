#pragma once

#include <string>

namespace vt {

// Optional cleanup stage applied to the final text before it is pasted.
// In the MVP this is a no-op; the interface exists so an LLM/HTTPS cleanup
// backend can be added later without touching the pipeline.
class ITextPostProcessor {
public:
    virtual ~ITextPostProcessor() = default;
    virtual std::string process(const std::string& text) = 0;
};

} // namespace vt
