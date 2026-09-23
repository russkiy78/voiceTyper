#include "asr/InitialPrompts.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace vt {

namespace {

// Well above dictation pace (about 15-20 normalized characters a second, 25
// when rushing). An echo is a prompt sentence decoded from a cough-length
// blip, i.e. 100+ characters a second.
constexpr double kMaxSpokenCharsPerSecond = 35.0;

// Lowercased words separated by single spaces; punctuation dropped, so an echo
// that differs from the prompt only in punctuation or casing still matches.
QString normalizedWords(const QString& s) {
    QString out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) {
            if (pendingSpace && !out.isEmpty())
                out += QLatin1Char(' ');
            pendingSpace = false;
            out += c.toLower();
        } else {
            pendingSpace = true;
        }
    }
    return out;
}

} // namespace

bool parseInitialPrompts(const QString& json, InitialPrompts* out,
                         QString* error) {
    out->clear();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("JSON parse error at offset %1: %2")
                         .arg(perr.offset)
                         .arg(perr.errorString());
        return false;
    }
    const QJsonValue prompts =
        doc.isObject() ? doc.object().value(QStringLiteral("prompts"))
                       : QJsonValue();
    if (!prompts.isObject()) {
        if (error)
            *error = QStringLiteral(
                "Config must be an object with a \"prompts\" object.");
        return false;
    }

    const QJsonObject obj = prompts.toObject();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().isString())
            out->insert(it.key(), it.value().toString().trimmed());
    }
    return true;
}

QString initialPromptKey(const QString& language, bool translate) {
    if (translate)
        return QStringLiteral("en");
    if (language.isEmpty() || language == QLatin1String("auto"))
        return QStringLiteral("auto");
    return language;
}

QString pickInitialPrompt(const InitialPrompts& bundled,
                          const InitialPrompts& user, const QString& language,
                          bool translate) {
    const QString key = initialPromptKey(language, translate);
    return user.contains(key) ? user.value(key) : bundled.value(key);
}

bool looksLikePromptEcho(const std::string& text, const std::string& prompt,
                         double speechSeconds) {
    const QString t = normalizedWords(QString::fromStdString(text));
    const QString p = normalizedWords(QString::fromStdString(prompt));
    if (t.isEmpty() || p.isEmpty() || t.count(QLatin1Char(' ')) < 2)
        return false; // fewer than three words
    // At least 40% of the prompt: a whole leaked sentence, not a common phrase.
    if (t.size() * 5 < p.size() * 2)
        return false;
    if (t.size() <= kMaxSpokenCharsPerSecond * speechSeconds)
        return false; // could have been spoken: trust it
    return (QLatin1Char(' ') + p + QLatin1Char(' '))
        .contains(QLatin1Char(' ') + t + QLatin1Char(' '));
}

} // namespace vt
