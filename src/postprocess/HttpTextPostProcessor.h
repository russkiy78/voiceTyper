#pragma once

#include "postprocess/TextPostProcessor.h"

#include <string>

namespace vt {

// Skeleton for a future external HTTPS cleanup endpoint (e.g. an LLM that tidies
// punctuation/casing). Not wired up in the MVP — process() is a pass-through.
//
// TODO (post-MVP): perform a synchronous POST to endpoint_ with the text using
// QNetworkAccessManager driven by a local QEventLoop (this runs on a worker
// thread, so blocking is acceptable), parse the JSON reply, and fall back to the
// original text on any timeout/error so dictation never breaks.
class HttpTextPostProcessor final : public ITextPostProcessor {
public:
    explicit HttpTextPostProcessor(std::string endpoint, int timeoutMs = 4000)
        : endpoint_(std::move(endpoint)), timeoutMs_(timeoutMs) {}

    std::string process(const std::string& text) override {
        // TODO: replace with real HTTPS round-trip; pass-through for now.
        return text;
    }

private:
    std::string endpoint_;
    int timeoutMs_;
};

} // namespace vt
