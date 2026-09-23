// Standalone correctness check for the initial-prompt config (InitialPrompts)
// and the VAD speech shaping (SpeechCompaction). Not part of the app build;
// requires only Qt6Core. Run from the repo root (it reads the bundled
// config/prompts.default.json); build with:
//   g++ -std=c++17 -fPIC -Isrc tests/initial_prompts_test.cpp
//       src/asr/InitialPrompts.cpp src/asr/SpeechCompaction.cpp
//       $(pkg-config --cflags --libs Qt6Core) -o initial_prompts_test

#include "asr/InitialPrompts.h"
#include "asr/SpeechCompaction.h"

#include <QFile>

#include <cstdio>
#include <string>
#include <vector>

using namespace vt;

static int g_failures = 0;

static void check(const std::string& name, const QString& got,
                  const QString& want) {
    const bool ok = got == want;
    if (!ok)
        ++g_failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("   got : %s\n", got.toUtf8().constData());
        std::printf("   want: %s\n", want.toUtf8().constData());
    }
}

static void checkBool(const std::string& name, bool got, bool want) {
    const bool ok = got == want;
    if (!ok)
        ++g_failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
}

// Samples are their own index, so the output shows exactly which were kept.
static std::vector<float> ramp(int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = static_cast<float>(i);
    return v;
}

static std::vector<float> range(int from, int to) {
    std::vector<float> v;
    for (int i = from; i < to; ++i)
        v.push_back(static_cast<float>(i));
    return v;
}

static std::vector<float> cat(std::initializer_list<std::vector<float>> parts) {
    std::vector<float> v;
    for (const auto& p : parts)
        v.insert(v.end(), p.begin(), p.end());
    return v;
}

static void checkSamples(const std::string& name, const std::vector<float>& got,
                         const std::vector<float>& want) {
    const bool ok = got == want;
    if (!ok)
        ++g_failures;
    std::printf("[%s] %s (got %zu samples, want %zu)\n", ok ? "PASS" : "FAIL",
                name.c_str(), got.size(), want.size());
}

int main() {
    // --- Prompt config ---------------------------------------------------
    QFile file(QStringLiteral("config/prompts.default.json"));
    checkBool("bundled_file_opens", file.open(QIODevice::ReadOnly), true);
    InitialPrompts bundled;
    QString err;
    checkBool("bundled_file_parses",
              parseInitialPrompts(QString::fromUtf8(file.readAll()), &bundled, &err),
              true);
    for (const char* lang : {"ru", "en", "uk", "de", "fr", "es", "it", "pl", "pt", "nl"})
        checkBool(std::string("bundled_has_") + lang,
                  !bundled.value(QString::fromLatin1(lang)).isEmpty(), true);
    check("bundled_auto_is_empty", bundled.value(QStringLiteral("auto")), QString());

    InitialPrompts parsed;
    checkBool("invalid_json_rejected", parseInitialPrompts("{ nope", &parsed, &err), false);
    checkBool("missing_prompts_rejected", parseInitialPrompts(R"({"x": 1})", &parsed, &err), false);
    checkBool("non_string_ignored",
              parseInitialPrompts(R"({"prompts": {"ru": 5, "en": "  Hi.  "}})", &parsed, &err) &&
                  !parsed.contains("ru") && parsed.value("en") == "Hi.",
              true);

    check("key_language", initialPromptKey("ru", false), "ru");
    check("key_auto", initialPromptKey("auto", false), "auto");
    check("key_empty_is_auto", initialPromptKey("", false), "auto");
    check("key_translate_is_en", initialPromptKey("ru", true), "en");

    InitialPrompts user;
    parseInitialPrompts(R"({"prompts": {"ru": "Мой стиль.", "de": "", "auto": "Привет, pull request."}})",
                        &user, &err);
    check("user_overrides", pickInitialPrompt(bundled, user, "ru", false), "Мой стиль.");
    check("user_empty_disables", pickInitialPrompt(bundled, user, "de", false), QString());
    check("user_auto", pickInitialPrompt(bundled, user, "auto", false), "Привет, pull request.");
    check("missing_key_falls_back", pickInitialPrompt(bundled, user, "en", false),
          bundled.value("en"));
    check("unknown_language_no_prompt", pickInitialPrompt(bundled, user, "ja", false), QString());

    // --- Prompt echo -----------------------------------------------------
    // What an echo decodes from: a cough-length blip (plus the VAD's padding).
    const double blip = 0.8;
    const std::string prompt = bundled.value("ru").toStdString();
    checkBool("echo_whole_prompt", looksLikePromptEcho(prompt, prompt, blip), true);
    checkBool("echo_ignores_case_and_punct",
              looksLikePromptEcho("добрый день сегодня обсудим план работы что уже "
                                  "сделано что осталось и когда мы закончим",
                                  prompt, blip),
              true);
    checkBool("echo_leaked_sentence",
              looksLikePromptEcho("Сегодня обсудим план работы: что уже сделано, что "
                                  "осталось и когда мы закончим.",
                                  prompt, blip),
              true);
    checkBool("short_phrase_from_prompt_kept",
              looksLikePromptEcho("Главное — не торопиться.", prompt, blip), false);
    checkBool("two_words_kept", looksLikePromptEcho("Добрый день.", prompt, blip), false);
    checkBool("real_speech_kept",
              looksLikePromptEcho("Сегодня обсудим план работы и бюджет на следующий "
                                  "квартал, а потом закончим.",
                                  prompt, blip),
              false);
    checkBool("no_prompt_no_echo", looksLikePromptEcho("Добрый день.", "", blip), false);
    checkBool("partial_word_not_echo",
              looksLikePromptEcho("обрый день сегодня обсудим план работы что уже "
                                  "сделано что осталось и когда мы",
                                  prompt, blip),
              false);

    // Saying the prompt itself at dictation pace is speech, not an echo.
    checkBool("prompt_spoken_at_normal_pace_kept",
              looksLikePromptEcho(prompt, prompt, 8.0), false);
    const std::string shortPrompt = "Привет. Как дела?";
    checkBool("short_user_prompt_spoken_kept",
              looksLikePromptEcho("Привет, как дела?", shortPrompt, 1.5), false);
    checkBool("short_user_prompt_echo_on_blip",
              looksLikePromptEcho("Привет, как дела?", shortPrompt, 0.3), true);
    // "привет как дела" is 15 normalized chars: echo only above 35 chars/s.
    checkBool("rate_just_under_limit_kept",
              looksLikePromptEcho("Привет, как дела?", shortPrompt, 0.45), false);
    checkBool("rate_just_over_limit_echo",
              looksLikePromptEcho("Привет, как дела?", shortPrompt, 0.4), true);

    // --- Speech compaction ----------------------------------------------
    const std::vector<float> audio = ramp(1000);
    checkSamples("no_spans_empty", compactSpeech(audio, {}, 10, 50), {});
    checkSamples("edges_padded", compactSpeech(audio, {{100, 200}}, 10, 50),
                 range(90, 210));
    checkSamples("edges_clamped", compactSpeech(audio, {{5, 995}}, 10, 50),
                 range(0, 1000));
    checkSamples("short_pause_kept", compactSpeech(audio, {{100, 200}, {240, 300}}, 10, 50),
                 range(90, 310));
    checkSamples("long_pause_shortened",
                 compactSpeech(audio, {{100, 200}, {500, 600}}, 10, 50),
                 cat({range(90, 200), range(200, 225), range(475, 500), range(500, 610)}));
    checkSamples("overlap_merged", compactSpeech(audio, {{100, 300}, {250, 400}}, 10, 50),
                 range(90, 410));
    checkSamples("contained_span_skipped",
                 compactSpeech(audio, {{100, 400}, {150, 200}}, 10, 50), range(90, 410));
    checkSamples("unsorted_input", compactSpeech(audio, {{500, 600}, {100, 200}}, 10, 50),
                 cat({range(90, 200), range(200, 225), range(475, 500), range(500, 610)}));
    checkSamples("out_of_range_dropped", compactSpeech(audio, {{1200, 1300}}, 10, 50), {});

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
