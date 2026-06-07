#include "commands/CommandEngine.h"

#include <algorithm>

namespace vt {

namespace {

bool isTextInsertingAction(CommandAction a) {
    return a == CommandAction::InsertText || a == CommandAction::InsertSymbol;
}

// Output assembler that joins plain words with single spaces while letting
// command payloads (\n, \t, ...) sit flush against their neighbours.
//   word RAW("\n") word  ->  "word\nword"   (no spaces around the payload)
//   word word            ->  "word word"
class OutputBuilder {
public:
    void addWord(const QString& w) {
        if (prevWasWord_)
            out_ += QLatin1Char(' ');
        out_ += w;
        prevWasWord_ = true;
    }
    void addRaw(const QString& v) {
        out_ += v;
        prevWasWord_ = false;
    }
    [[nodiscard]] QString result() const {
        // Trim only ASCII spaces at the edges; keep intentional \n / \t payloads.
        int a = 0;
        int b = out_.size();
        while (a < b && out_.at(a) == QLatin1Char(' '))
            ++a;
        while (b > a && out_.at(b - 1) == QLatin1Char(' '))
            --b;
        return out_.mid(a, b - a);
    }

private:
    QString out_;
    bool prevWasWord_ = false;
};

} // namespace

CommandEngine::CommandEngine(const CommandConfig& config) { setConfig(config); }

void CommandEngine::setConfig(const CommandConfig& config) {
    config_ = config;
    rebuild();
}

QString CommandEngine::normalizeToken(const QString& token) {
    QString out;
    out.reserve(token.size());
    for (const QChar c : token) {
        if (c.isLetterOrNumber())
            out += c.toLower();
    }
    return out;
}

QStringList CommandEngine::normalizedTokens(const QString& text) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList raw = text.split(ws, Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(raw.size());
    for (const QString& r : raw) {
        const QString n = normalizeToken(r);
        if (!n.isEmpty())
            out << n;
    }
    return out;
}

QString CommandEngine::normalizeForRegex(const QString& text) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return text.toLower().simplified().replace(ws, QStringLiteral(" "));
}

void CommandEngine::rebuild() {
    phrasePatterns_.clear();
    regexPatterns_.clear();

    for (const CommandDefinition& cmd : config_.commands) {
        if (!cmd.enabled)
            continue;

        if (cmd.matchMode == MatchMode::Regex) {
            for (const QString& pat : cmd.patterns) {
                QRegularExpression re(
                    pat, QRegularExpression::CaseInsensitiveOption |
                             QRegularExpression::UseUnicodePropertiesOption);
                if (re.isValid())
                    regexPatterns_.push_back({&cmd, std::move(re)});
            }
        } else { // NormalizedPhrase
            for (const QString& pat : cmd.patterns) {
                const QStringList tokens = normalizedTokens(pat);
                if (!tokens.isEmpty())
                    phrasePatterns_.push_back({&cmd, tokens});
            }
        }
    }

    // Greedy longest-match: try patterns with more tokens first.
    std::stable_sort(phrasePatterns_.begin(), phrasePatterns_.end(),
                     [](const PhrasePattern& a, const PhrasePattern& b) {
                         return a.tokens.size() > b.tokens.size();
                     });
}

CommandProcessingResult CommandEngine::processFinalText(const QString& input) const {
    CommandProcessingResult result;

    // --- Phase 1: normalized-phrase pass (token based) ----------------------
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList words = input.split(ws, Qt::SkipEmptyParts);
    const int n = words.size();

    std::vector<QString> norm;
    norm.reserve(n);
    for (const QString& w : words)
        norm.push_back(normalizeToken(w));

    OutputBuilder builder;
    bool stop = false;

    int i = 0;
    while (i < n) {
        const CommandDefinition* matched = nullptr;
        int matchLen = 0;
        for (const PhrasePattern& pp : phrasePatterns_) {
            const int len = pp.tokens.size();
            if (len == 0 || i + len > n || len <= matchLen)
                continue;
            bool eq = true;
            for (int k = 0; k < len; ++k) {
                if (norm[i + k] != pp.tokens[k]) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                matched = pp.cmd;
                matchLen = len;
            }
        }

        if (!matched) {
            builder.addWord(words[i]);
            ++i;
            continue;
        }

        if (matched->action == CommandAction::StopRecording) {
            stop = true;
            if (matched->removeFromOutput) {
                // Drop the phrase and everything after it: audio past "stop"
                // must not reach the output.
                break;
            }
            for (int k = 0; k < matchLen; ++k)
                builder.addWord(words[i + k]);
            i += matchLen;
        } else if (isTextInsertingAction(matched->action)) {
            if (matched->removeFromOutput)
                builder.addRaw(matched->value);
            else
                for (int k = 0; k < matchLen; ++k)
                    builder.addWord(words[i + k]);
            i += matchLen;
        } else {
            // Other actions are not yet applied to text; keep the words.
            for (int k = 0; k < matchLen; ++k)
                builder.addWord(words[i + k]);
            i += matchLen;
        }
    }

    QString text = builder.result();

    // --- Phase 2: regex pass (operates on the reassembled string) -----------
    // TODO: regex patterns whose replacement inserts whitespace are not
    // re-tokenized; for whitespace commands prefer normalized_phrase mode.
    for (const RegexPattern& rp : regexPatterns_) {
        if (rp.cmd->action == CommandAction::StopRecording) {
            const QRegularExpressionMatch m = rp.re.match(text);
            if (m.hasMatch()) {
                stop = true;
                if (rp.cmd->removeFromOutput)
                    text = text.left(m.capturedStart());
            }
        } else if (isTextInsertingAction(rp.cmd->action)) {
            if (rp.cmd->removeFromOutput)
                text.replace(rp.re, rp.cmd->value);
        }
    }

    result.text = text.toStdString();
    result.requestedStop = stop;
    return result;
}

bool CommandEngine::containsStopCommand(const QString& partialText) const {
    const QStringList norm = normalizedTokens(partialText);
    const int n = norm.size();

    for (const PhrasePattern& pp : phrasePatterns_) {
        if (pp.cmd->action != CommandAction::StopRecording)
            continue;
        const int len = pp.tokens.size();
        if (len == 0 || len > n)
            continue;
        for (int i = 0; i + len <= n; ++i) {
            bool eq = true;
            for (int k = 0; k < len; ++k) {
                if (norm[i + k] != pp.tokens[k]) {
                    eq = false;
                    break;
                }
            }
            if (eq)
                return true;
        }
    }

    if (!regexPatterns_.empty()) {
        const QString flat = normalizeForRegex(partialText);
        for (const RegexPattern& rp : regexPatterns_) {
            if (rp.cmd->action == CommandAction::StopRecording &&
                rp.re.match(flat).hasMatch())
                return true;
        }
    }

    return false;
}

CommandProcessingResult CommandEngine::processFinalText(const std::string& input) const {
    return processFinalText(QString::fromStdString(input));
}

bool CommandEngine::containsStopCommand(const std::string& partialText) const {
    return containsStopCommand(QString::fromStdString(partialText));
}

} // namespace vt
