#pragma once

#include "commands/CommandTypes.h"

#include <QString>
#include <QVector>

namespace vt {

// Parsed commands configuration. Loaded from / saved to the JSON file owned by
// SettingsStore. Unknown action strings and match modes are preserved so a
// newer config does not get silently corrupted by an older binary.
class CommandConfig {
public:
    QVector<CommandDefinition> commands;

    // Parses JSON text. On failure returns an empty config and fills *error.
    static CommandConfig fromJson(const QString& json, QString* error = nullptr);

    // Serializes back to pretty-printed JSON.
    [[nodiscard]] QString toJson() const;

    // Validates JSON without applying it. Returns true if parseable.
    static bool validate(const QString& json, QString* error = nullptr);
};

} // namespace vt
