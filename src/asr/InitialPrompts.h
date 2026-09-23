#pragma once

#include <QHash>
#include <QString>

#include <string>

namespace vt {

// Whisper initial prompts by language key ("ru", "en", ..., "auto"), as stored
// in prompts.json. Whisper treats the prompt as text preceding the recording
// and copies its punctuation/casing style, which keeps that style stable from
// one dictation to the next.
using InitialPrompts = QHash<QString, QString>;

// Parses {"prompts": {"<language>": "<text>", ...}}. Non-string entries are
// ignored. On malformed input returns false and fills *error.
bool parseInitialPrompts(const QString& json, InitialPrompts* out,
                         QString* error = nullptr);

// Key of the prompt a transcription should use: translation outputs English,
// so it takes "en"; an auto-detected language takes "auto"; otherwise the
// language code itself.
QString initialPromptKey(const QString& language, bool translate);

// The user's entry wins key by key (an empty string disables the prompt);
// keys the user file lacks fall back to the bundled defaults, so languages
// added in later versions still get one.
QString pickInitialPrompt(const InitialPrompts& bundled,
                          const InitialPrompts& user, const QString& language,
                          bool translate);

// True when `text` is whisper repeating its prompt instead of transcribing
// speech — the typical output for a noise it has no words for. Requires a
// multi-word match covering a sizeable part of the prompt, and more text than
// anyone could say in `speechSeconds` of audio: a phrase the user really said
// is kept even when it matches the prompt word for word.
bool looksLikePromptEcho(const std::string& text, const std::string& prompt,
                         double speechSeconds);

} // namespace vt
