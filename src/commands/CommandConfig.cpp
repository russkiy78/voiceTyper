#include "commands/CommandConfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace vt {

// --- enum <-> string helpers (declared in CommandTypes.h) -------------------

QString toString(CommandAction action) {
    switch (action) {
    case CommandAction::InsertText: return "insert_text";
    case CommandAction::StopRecording: return "stop_recording";
    case CommandAction::InsertSymbol: return "insert_symbol";
    case CommandAction::DeletePreviousWord: return "delete_previous_word";
    case CommandAction::Submit: return "submit";
    case CommandAction::Escape: return "escape";
    case CommandAction::CustomHotkey: return "custom_hotkey";
    case CommandAction::CustomTextTransform: return "custom_text_transform";
    case CommandAction::Unknown: return "unknown";
    }
    return "unknown";
}

CommandAction commandActionFromString(const QString& s, bool* recognised) {
    static const struct {
        const char* name;
        CommandAction action;
    } table[] = {
        {"insert_text", CommandAction::InsertText},
        {"stop_recording", CommandAction::StopRecording},
        {"insert_symbol", CommandAction::InsertSymbol},
        {"delete_previous_word", CommandAction::DeletePreviousWord},
        {"submit", CommandAction::Submit},
        {"escape", CommandAction::Escape},
        {"custom_hotkey", CommandAction::CustomHotkey},
        {"custom_text_transform", CommandAction::CustomTextTransform},
    };
    for (const auto& e : table) {
        if (s == QLatin1String(e.name)) {
            if (recognised)
                *recognised = true;
            return e.action;
        }
    }
    if (recognised)
        *recognised = false;
    return CommandAction::Unknown;
}

QString toString(MatchMode mode) {
    return mode == MatchMode::Regex ? "regex" : "normalized_phrase";
}

MatchMode matchModeFromString(const QString& s) {
    return s == QLatin1String("regex") ? MatchMode::Regex
                                       : MatchMode::NormalizedPhrase;
}

// --- CommandConfig ----------------------------------------------------------

CommandConfig CommandConfig::fromJson(const QString& json, QString* error) {
    CommandConfig config;

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("JSON parse error at offset %1: %2")
                         .arg(perr.offset)
                         .arg(perr.errorString());
        return config;
    }
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("Top-level JSON must be an object.");
        return config;
    }

    const QJsonArray arr = doc.object().value("commands").toArray();
    for (const QJsonValue& v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();

        CommandDefinition cmd;
        cmd.id = o.value("id").toString();
        cmd.enabled = o.value("enabled").toBool(true);

        cmd.rawAction = o.value("action").toString();
        bool recognised = false;
        cmd.action = commandActionFromString(cmd.rawAction, &recognised);

        cmd.value = o.value("value").toString();
        cmd.matchMode = matchModeFromString(o.value("match_mode").toString());
        cmd.removeFromOutput = o.value("remove_command_from_output").toBool(true);

        for (const QJsonValue& p : o.value("patterns").toArray()) {
            const QString s = p.toString();
            if (!s.isEmpty())
                cmd.patterns << s;
        }

        config.commands.push_back(cmd);
    }

    return config;
}

QString CommandConfig::toJson() const {
    QJsonArray arr;
    for (const CommandDefinition& cmd : commands) {
        QJsonObject o;
        o.insert("id", cmd.id);
        o.insert("enabled", cmd.enabled);
        // Preserve the original action string for unknown/forward-compat actions.
        o.insert("action", cmd.action == CommandAction::Unknown
                               ? cmd.rawAction
                               : toString(cmd.action));
        if (cmd.action == CommandAction::InsertText ||
            cmd.action == CommandAction::InsertSymbol ||
            !cmd.value.isEmpty())
            o.insert("value", cmd.value);

        QJsonArray patterns;
        for (const QString& p : cmd.patterns)
            patterns.append(p);
        o.insert("patterns", patterns);

        o.insert("match_mode", toString(cmd.matchMode));
        o.insert("remove_command_from_output", cmd.removeFromOutput);
        arr.append(o);
    }

    QJsonObject root;
    root.insert("version", 1);
    root.insert("commands", arr);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool CommandConfig::validate(const QString& json, QString* error) {
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("JSON parse error at offset %1: %2")
                         .arg(perr.offset)
                         .arg(perr.errorString());
        return false;
    }
    if (!doc.isObject() || !doc.object().contains("commands")) {
        if (error)
            *error = QStringLiteral("Config must be an object with a \"commands\" array.");
        return false;
    }
    return true;
}

} // namespace vt
