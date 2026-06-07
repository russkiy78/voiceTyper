// Standalone correctness check for CommandEngine against the spec's acceptance
// scenarios. Not part of the app build; compile manually (see the bottom of
// README / the command used in development). Requires only Qt6Core.

#include "commands/CommandConfig.h"
#include "commands/CommandEngine.h"

#include <cstdio>
#include <string>

using namespace vt;

static int g_failures = 0;

static void check(const std::string& name, const std::string& got,
                  const std::string& want) {
    const bool ok = got == want;
    if (!ok)
        ++g_failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("   got : %s\n", got.c_str());
        std::printf("   want: %s\n", want.c_str());
    }
}

static void checkBool(const std::string& name, bool got, bool want) {
    const bool ok = got == want;
    if (!ok)
        ++g_failures;
    std::printf("[%s] %s (got %d want %d)\n", ok ? "PASS" : "FAIL", name.c_str(),
                got, want);
}

static const char* kDefaultConfig = R"JSON(
{
  "commands": [
    { "id": "newline", "enabled": true, "action": "insert_text", "value": "\n",
      "patterns": ["новая строка", "перенос строки", "следующая строка"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true },
    { "id": "stop", "enabled": true, "action": "stop_recording",
      "patterns": ["стоп", "остановить запись", "конец диктовки"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true },
    { "id": "tab", "enabled": true, "action": "insert_text", "value": "\t",
      "patterns": ["табуляция", "таб"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true },
    { "id": "comma", "enabled": true, "action": "insert_text", "value": ",",
      "patterns": ["запятая"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true },
    { "id": "period", "enabled": true, "action": "insert_text", "value": ".",
      "patterns": ["точка"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true }
  ]
}
)JSON";

static const char* kCustomNewline = R"JSON(
{
  "commands": [
    { "id": "newline", "enabled": true, "action": "insert_text", "value": "\n",
      "patterns": ["перенос"],
      "match_mode": "normalized_phrase", "remove_command_from_output": true }
  ]
}
)JSON";

int main() {
    QString err;
    CommandEngine engine(CommandConfig::fromJson(kDefaultConfig, &err));
    if (!err.isEmpty())
        std::printf("config parse error: %s\n", err.toStdString().c_str());

    // Scenario 2: «новая строка» → newline.
    check("scenario2_newline",
          engine.processFinalText(std::string("Привет новая строка это тест")).text,
          "Привет\nэто тест");

    // Scenario 7: command processing across a longer text.
    check("scenario7_multiline",
          engine.processFinalText(std::string(
              "Привет новая строка сегодня я проверяю голосовой ввод новая "
              "строка всё работает")).text,
          "Привет\nсегодня я проверяю голосовой ввод\nвсё работает");

    // Tab / comma / period commands.
    check("tab", engine.processFinalText(std::string("раз табуляция два")).text,
          "раз\tдва");
    check("comma", engine.processFinalText(std::string("раз запятая два")).text,
          "раз,два");
    check("period", engine.processFinalText(std::string("раз точка")).text,
          "раз.");

    // Punctuation on tokens should not break matching ("строка," → "строка").
    check("newline_with_punct",
          engine.processFinalText(std::string("привет новая строка, это тест")).text,
          "привет\nэто тест");

    // Scenario 4: stop word is detected live and removed from final text.
    checkBool("stop_detect_live",
              engine.containsStopCommand(std::string("это тест стоп")), true);
    checkBool("stop_detect_absent",
              engine.containsStopCommand(std::string("это просто текст")), false);

    {
        const auto r = engine.processFinalText(
            std::string("привет это тест стоп лишний хвост"));
        check("stop_truncates", r.text, "привет это тест");
        checkBool("stop_flag", r.requestedStop, true);
    }

    // Scenario 3: user-defined phrase «перенос» means newline.
    {
        CommandEngine custom(CommandConfig::fromJson(kCustomNewline, &err));
        check("scenario3_custom_newline",
              custom.processFinalText(std::string("Привет перенос это тест")).text,
              "Привет\nэто тест");
    }

    // Multi-word stop phrase.
    {
        const auto r = engine.processFinalText(
            std::string("текст готов конец диктовки дальше игнор"));
        check("multiword_stop", r.text, "текст готов");
        checkBool("multiword_stop_flag", r.requestedStop, true);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
