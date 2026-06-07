#pragma once

#include "commands/CommandConfig.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace vt {

struct CommandProcessingResult {
    std::string text;
    bool requestedStop = false;
};

// Transforms recognised speech into final text by applying configured commands.
//
// Two matching strategies:
//   * normalized_phrase - token-based; lowercase + punctuation-stripped phrase
//     comparison with greedy longest-match. This is the fully supported MVP path.
//   * regex             - QRegularExpression replacement applied after the
//     phrase pass (functional; see TODO about whitespace-inserting patterns).
//
// Works in QString internally for correct Unicode (Cyrillic) case-folding; the
// std::string overloads exist to match the interface in the spec.
class CommandEngine {
public:
    CommandEngine() = default;
    explicit CommandEngine(const CommandConfig& config);

    void setConfig(const CommandConfig& config);
    [[nodiscard]] const CommandConfig& config() const { return config_; }

    [[nodiscard]] CommandProcessingResult processFinalText(const std::string& input) const;
    [[nodiscard]] bool containsStopCommand(const std::string& partialText) const;

    [[nodiscard]] CommandProcessingResult processFinalText(const QString& input) const;
    [[nodiscard]] bool containsStopCommand(const QString& partialText) const;

    // Exposed for unit testing / reuse.
    static QString normalizeToken(const QString& token);
    static QStringList normalizedTokens(const QString& text);
    static QString normalizeForRegex(const QString& text);

private:
    struct PhrasePattern {
        const CommandDefinition* cmd = nullptr;
        QStringList tokens; // normalized
    };
    struct RegexPattern {
        const CommandDefinition* cmd = nullptr;
        QRegularExpression re;
    };

    void rebuild();

    CommandConfig config_;
    std::vector<PhrasePattern> phrasePatterns_;
    std::vector<RegexPattern> regexPatterns_;
};

} // namespace vt
