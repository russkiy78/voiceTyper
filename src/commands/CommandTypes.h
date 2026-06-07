#pragma once

#include <QString>
#include <QStringList>

namespace vt {

// Action performed when a command phrase is recognised.
// Only InsertText and StopRecording are implemented in the MVP; the rest are
// reserved so the config schema and engine stay forward-compatible.
enum class CommandAction {
    InsertText,
    StopRecording,
    // --- TODO (post-MVP) ---
    InsertSymbol,
    DeletePreviousWord,
    Submit,
    Escape,
    CustomHotkey,
    CustomTextTransform,
    Unknown, // action string not recognised; preserved verbatim on save
};

enum class MatchMode {
    NormalizedPhrase,
    Regex,
};

struct CommandDefinition {
    QString id;
    bool enabled = true;
    CommandAction action = CommandAction::Unknown;
    QString rawAction;  // original action string (round-trips unknown actions)
    QString value;      // payload for insert_text (already unescaped)
    QStringList patterns;
    MatchMode matchMode = MatchMode::NormalizedPhrase;
    bool removeFromOutput = true;
};

QString toString(CommandAction action);
CommandAction commandActionFromString(const QString& s, bool* recognised = nullptr);

QString toString(MatchMode mode);
MatchMode matchModeFromString(const QString& s);

} // namespace vt
