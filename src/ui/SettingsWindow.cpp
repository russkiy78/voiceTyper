#include "ui/SettingsWindow.h"

#include "asr/ComputeBackends.h"
#include "audio/ReminderBeep.h"
#include "commands/CommandConfig.h"
#include "postprocess/LlmPostProcessor.h"
#include "settings/SettingsStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace vt {

namespace {
struct Lang {
    const char* code;
    const char* name;
};
const Lang kLanguages[] = {
    {"auto", "Auto-detect"}, {"ru", "Russian"},  {"en", "English"},
    {"uk", "Ukrainian"},     {"de", "German"},   {"fr", "French"},
    {"es", "Spanish"},       {"it", "Italian"},  {"pl", "Polish"},
    {"pt", "Portuguese"},    {"nl", "Dutch"},    {"tr", "Turkish"},
    {"cs", "Czech"},         {"zh", "Chinese"},  {"ja", "Japanese"},
    {"ko", "Korean"},        {"ar", "Arabic"},
};
} // namespace

SettingsWindow::SettingsWindow(SettingsStore* settings, QWidget* parent)
    : QDialog(parent), settings_(settings),
      llmTester_(new LlmPostProcessor(this)) {
    setWindowTitle(tr("voiceTyper — Settings"));
    setMinimumSize(560, 770);
    buildUi();
    loadFromSettings();
    // Keep the mode checkboxes in sync with external changes (hotkey toggles).
    connect(settings_, &SettingsStore::changed, this, [this]() {
        translate_->setChecked(settings_->translate());
        llmEnabled_->setChecked(settings_->llmEnabled());
    });
}

void SettingsWindow::showEvent(QShowEvent* event) {
    if (!event->spontaneous())
        loadFromSettings();
    QDialog::showEvent(event);
}

void SettingsWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    root->addWidget(tabs_, 1);

    auto* general = new QWidget(this);
    auto* generalLayout = new QVBoxLayout(general);

    // --- General -------------------------------------------------------
    auto* form = new QFormLayout();

    language_ = new QComboBox(this);
    for (const Lang& l : kLanguages)
        language_->addItem(tr(l.name), QString::fromLatin1(l.code));
    form->addRow(tr("Recognition language:"), language_);

    translate_ = new QCheckBox(tr("Translate to English"), this);
    form->addRow(QString(), translate_);

    vadEnabled_ = new QCheckBox(tr("Decode only detected speech (VAD)"), this);
    vadEnabled_->setToolTip(
        tr("The Silero VAD cuts silence before Whisper runs, so silence is "
           "not turned into phantom phrases. Turn off if words or whole "
           "dictations go missing: recordings are then decoded whole."));
    if (SettingsStore::vadModelPath().isEmpty()) {
        vadEnabled_->setEnabled(false);
        vadEnabled_->setToolTip(
            tr("The VAD model is missing from this install; recordings are "
               "decoded whole."));
    }
    form->addRow(QString(), vadEnabled_);

    hotkey_ = new QKeySequenceEdit(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    hotkey_->setMaximumSequenceLength(1);
#endif
    form->addRow(tr("Global hotkey:"), hotkey_);

    translateHotkey_ = new QKeySequenceEdit(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    translateHotkey_->setMaximumSequenceLength(1);
#endif
    form->addRow(tr("Translation toggle hotkey:"), translateHotkey_);

    // Model path + browse button.
    modelPath_ = new QLineEdit(this);
    auto* browse = new QPushButton(tr("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, &SettingsWindow::browseModel);
    auto* modelRow = new QHBoxLayout();
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->addWidget(modelPath_, 1);
    modelRow->addWidget(browse);
    form->addRow(tr("Whisper model:"), modelRow);

    // Link for model downloads as a separate form row — avoids the QFormLayout
    // squishing bug that occurs when a multi-row QWidget wrapper is used as a
    // field widget and the computeBackend combo triggers a layout recalculation.
    auto* modelLink =
        new QLabel(tr("<a href=\"#\">Download models</a>"), this);
    modelLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    modelLink->setStyleSheet(QStringLiteral("font-size: 11px;"));
    connect(modelLink, &QLabel::linkActivated, this,
            &SettingsWindow::openModelDownloadsDialog);
    form->addRow(QString(), modelLink);

    // Compute backend: CPU is always present; Vulkan/CUDA entries appear only
    // when the build includes that backend AND a live device is detected.
    computeBackend_ = new QComboBox(this);
    for (const ComputeDevice& d : enumerateComputeDevices()) {
        const QString label =
            d.isGpu ? QString::fromStdString(d.backendName + " — " + d.deviceName)
                    : tr("CPU");
        computeBackend_->addItem(label, QString::fromStdString(d.id));
    }
    form->addRow(tr("Compute backend:"), computeBackend_);



    overlayEnabled_ = new QCheckBox(tr("Show recording overlay"), this);
    form->addRow(QString(), overlayEnabled_);

    reminderBeep_ = new QCheckBox(
        tr("Beep every %1 s while recording").arg(ReminderBeep::kIntervalSeconds),
        this);
    reminderBeep_->setToolTip(
        tr("A short, quiet beep reminds you that dictation is still recording, "
           "in case you forgot to stop it."));
    form->addRow(QString(), reminderBeep_);

    clipboardDelay_ = new QSpinBox(this);
    clipboardDelay_->setRange(0, 5000);
    clipboardDelay_->setSuffix(tr(" ms"));
    clipboardDelay_->setSingleStep(50);
    form->addRow(tr("Clipboard restore delay:"), clipboardDelay_);

    logEnabled_ = new QCheckBox(tr("Write a diagnostic log file"), this);
    logEnabled_->setToolTip(
        tr("Logs to voicetyper.log in the app config folder. Useful for "
           "diagnosing crashes (e.g. GPU init failures)."));
    form->addRow(QString(), logEnabled_);

    generalLayout->addLayout(form);

    // --- Command detection loop ---------------------------------------
    auto* detectGroup = new QGroupBox(tr("Voice stop detection (while recording)"), this);
    auto* cdForm = new QFormLayout(detectGroup);

    cdEnabled_ = new QCheckBox(tr("Detect stop command during recording"), this);
    cdForm->addRow(QString(), cdEnabled_);

    cdInterval_ = new QSpinBox(this);
    cdInterval_->setRange(500, 10000);
    cdInterval_->setSuffix(tr(" ms"));
    cdInterval_->setSingleStep(250);
    cdForm->addRow(tr("Sampling interval:"), cdInterval_);

    cdWindow_ = new QDoubleSpinBox(this);
    cdWindow_->setRange(1.0, 15.0);
    cdWindow_->setSuffix(tr(" s"));
    cdWindow_->setSingleStep(0.5);
    cdForm->addRow(tr("Tail window analysed:"), cdWindow_);

    generalLayout->addWidget(detectGroup);

    // --- Commands JSON editor -----------------------------------------
    auto* cmdGroup = new QGroupBox(tr("Commands (JSON)"), this);
    auto* cmdLayout = new QVBoxLayout(cmdGroup);

    commandsEditor_ = new QPlainTextEdit(this);
    commandsEditor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    commandsEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    cmdLayout->addWidget(commandsEditor_, 1);

    auto* cmdButtons = new QHBoxLayout();
    auto* validateBtn = new QPushButton(tr("Validate"), this);
    connect(validateBtn, &QPushButton::clicked, this,
            &SettingsWindow::validateCommands);
    commandsStatus_ = new QLabel(this);
    commandsStatus_->setWordWrap(true);
    cmdButtons->addWidget(validateBtn);
    cmdButtons->addWidget(commandsStatus_, 1);
    cmdLayout->addLayout(cmdButtons);

    generalLayout->addWidget(cmdGroup, 1);

    tabs_->addTab(general, tr("General"));
    llmTab_ = buildLlmTab();
    tabs_->addTab(llmTab_, tr("LLM"));

    // --- Dialog buttons -----------------------------------------------
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsWindow::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    root->addWidget(buttons);
}

QWidget* SettingsWindow::buildLlmTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* intro = new QLabel(
        tr("Sends the dictated text to an OpenAI-compatible chat-completions "
           "endpoint and pastes the model's answer instead of the text. If "
           "the request fails, the dictated text is pasted unchanged."),
        page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();

    llmEnabled_ = new QCheckBox(tr("Process dictated text with the LLM"), page);
    form->addRow(QString(), llmEnabled_);

    llmHotkey_ = new QKeySequenceEdit(page);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    llmHotkey_->setMaximumSequenceLength(1);
#endif
    form->addRow(tr("LLM toggle hotkey:"), llmHotkey_);

    llmEndpoint_ = new QLineEdit(page);
    llmEndpoint_->setPlaceholderText(
        QStringLiteral("https://api.example.com/v1/chat/completions"));
    llmEndpoint_->setToolTip(
        tr("Full URL of the chat-completions endpoint of any OpenAI-compatible "
           "server: a cloud API, or a local Ollama, llama.cpp server or LM "
           "Studio."));
    form->addRow(tr("Endpoint URL:"), llmEndpoint_);

    llmApiKey_ = new QLineEdit(page);
    llmApiKey_->setEchoMode(QLineEdit::Password);
    llmApiKey_->setPlaceholderText(tr("Optional"));
    llmApiKey_->setToolTip(
        tr("Sent as \"Authorization: Bearer <key>\"; leave empty for a server "
           "without authentication. Stored unencrypted in the app settings."));
    auto* showKey = new QCheckBox(tr("Show"), page);
    connect(showKey, &QCheckBox::toggled, this, [this](bool on) {
        llmApiKey_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto* keyRow = new QHBoxLayout();
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(llmApiKey_, 1);
    keyRow->addWidget(showKey);
    form->addRow(tr("API key:"), keyRow);

    llmModel_ = new QLineEdit(page);
    llmModel_->setPlaceholderText(tr("Optional — left out of the request when empty"));
    form->addRow(tr("Model:"), llmModel_);

    llmTimeout_ = new QSpinBox(page);
    llmTimeout_->setRange(5, 600);
    llmTimeout_->setSuffix(tr(" s"));
    llmTimeout_->setToolTip(
        tr("How long to wait for the answer before pasting the dictated text "
           "unchanged."));
    form->addRow(tr("Timeout:"), llmTimeout_);

    layout->addLayout(form);

    // --- Prompt --------------------------------------------------------
    auto* promptGroup = new QGroupBox(tr("Prompt"), page);
    auto* promptLayout = new QVBoxLayout(promptGroup);

    llmPrompt_ = new QPlainTextEdit(page);
    promptLayout->addWidget(llmPrompt_, 1);

    auto* promptHint = new QLabel(
        tr("Sent as the user message; {{text}} is replaced by the dictated "
           "text. Without {{text}}, the text is appended to the prompt."),
        page);
    promptHint->setWordWrap(true);
    promptHint->setStyleSheet(QStringLiteral("font-size: 11px;"));
    auto* resetPrompt = new QPushButton(tr("Default prompt"), page);
    connect(resetPrompt, &QPushButton::clicked, this, [this]() {
        llmPrompt_->setPlainText(SettingsStore::defaultLlmPrompt());
    });
    auto* promptRow = new QHBoxLayout();
    promptRow->addWidget(promptHint, 1);
    promptRow->addWidget(resetPrompt);
    promptLayout->addLayout(promptRow);

    layout->addWidget(promptGroup, 1);

    // --- Answer --------------------------------------------------------
    auto* answerForm = new QFormLayout();

    llmStripPattern_ = new QLineEdit(page);
    llmStripPattern_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    llmStripPattern_->setPlaceholderText(
        tr("Optional, e.g. %1").arg(QStringLiteral("<think>[\\s\\S]*?</think>")));
    llmStripPattern_->setToolTip(
        tr("Every match of this regular expression is removed from the answer "
           "before it is pasted — e.g. the model's reasoning or remarks. Empty: "
           "the whole answer is pasted."));
    answerForm->addRow(tr("Remove from answer (regex):"), llmStripPattern_);

    llmExtraParams_ = new QLineEdit(page);
    llmExtraParams_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    llmExtraParams_->setPlaceholderText(
        tr("Optional, e.g. %1")
            .arg(QStringLiteral("{\"temperature\": 0.2, \"max_tokens\": 4096}")));
    llmExtraParams_->setToolTip(
        tr("JSON object merged into the request body, for parameters the "
           "server supports (temperature, max_tokens, ...)."));
    answerForm->addRow(tr("Extra request fields (JSON):"), llmExtraParams_);

    layout->addLayout(answerForm);

    // --- Test ----------------------------------------------------------
    auto* testGroup = new QGroupBox(tr("Test (uses the fields above, unsaved)"), page);
    auto* testLayout = new QVBoxLayout(testGroup);

    llmTestInput_ = new QLineEdit(page);
    llmTestInput_->setText(
        tr("this is a test of the dictation cleanup it has no punctuation"));
    llmTestButton_ = new QPushButton(tr("Send"), page);
    connect(llmTestButton_, &QPushButton::clicked, this, &SettingsWindow::testLlm);
    connect(llmTestInput_, &QLineEdit::returnPressed, this, &SettingsWindow::testLlm);
    auto* testRow = new QHBoxLayout();
    testRow->addWidget(llmTestInput_, 1);
    testRow->addWidget(llmTestButton_);
    testLayout->addLayout(testRow);

    llmTestOutput_ = new QPlainTextEdit(page);
    llmTestOutput_->setReadOnly(true);
    llmTestOutput_->setPlaceholderText(tr("The text that would be pasted"));
    llmTestOutput_->setMaximumHeight(110);
    testLayout->addWidget(llmTestOutput_);

    layout->addWidget(testGroup);

    llmStatus_ = new QLabel(page);
    llmStatus_->setWordWrap(true);
    layout->addWidget(llmStatus_);

    return page;
}

void SettingsWindow::openModelDownloadsDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Download a Whisper model"));
    dlg.setMinimumWidth(380);

    auto* lay = new QVBoxLayout(&dlg);

    auto* intro = new QLabel(
        tr("Direct downloads from huggingface.co. Save the .bin file, then "
           "point \"Whisper model\" at it via Browse."),
        &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    // Recommended models — kept in sync with scripts/download-model.ps1.
    struct ModelDl {
        const char* name;
        const char* file;
        const char* size;
    };
    static const ModelDl kModels[] = {
        {"Small", "ggml-small-q5_1.bin", "~180 MB"},
        {"Medium", "ggml-medium-q5_0.bin", "~540 MB"},
        {"Large", "ggml-large-v3-q5_0.bin", "~1.1 GB"},
    };
    const QString base = QStringLiteral(
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/");

    for (const ModelDl& m : kModels) {
        auto* row = new QLabel(
            QStringLiteral("<a href=\"%1%2\">%3</a> &mdash; %4")
                .arg(base, QString::fromLatin1(m.file),
                     QString::fromLatin1(m.name), QString::fromLatin1(m.size)),
            &dlg);
        row->setOpenExternalLinks(true);
        row->setTextInteractionFlags(Qt::TextBrowserInteraction);
        lay->addWidget(row);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    dlg.exec();
}

void SettingsWindow::loadFromSettings() {
    const QString lang = settings_->language();
    int idx = language_->findData(lang);
    language_->setCurrentIndex(idx >= 0 ? idx : 0);

    translate_->setChecked(settings_->translate());
    vadEnabled_->setChecked(settings_->vadEnabled());
    hotkey_->setKeySequence(QKeySequence(settings_->hotkey(),
                                         QKeySequence::PortableText));
    translateHotkey_->setKeySequence(QKeySequence(settings_->translateHotkey(),
                                                   QKeySequence::PortableText));
    modelPath_->setText(settings_->modelPath());

    // Select the saved backend; if it's empty ("auto") or no longer available,
    // prefer the first GPU when present, otherwise CPU.
    int bidx = computeBackend_->findData(settings_->computeBackend());
    if (bidx < 0) {
        bidx = 0;
        for (int i = 0; i < computeBackend_->count(); ++i) {
            if (computeBackend_->itemData(i).toString() != QLatin1String("cpu")) {
                bidx = i;
                break;
            }
        }
    }
    computeBackend_->setCurrentIndex(bidx);


    overlayEnabled_->setChecked(settings_->overlayEnabled());
    reminderBeep_->setChecked(settings_->reminderBeepEnabled());
    logEnabled_->setChecked(settings_->loggingEnabled());
    clipboardDelay_->setValue(settings_->clipboardRestoreDelayMs());

    cdEnabled_->setChecked(settings_->commandDetectionEnabled());
    cdInterval_->setValue(settings_->commandDetectionIntervalMs());
    cdWindow_->setValue(settings_->commandDetectionWindowSeconds());

    commandsEditor_->setPlainText(settings_->loadCommandsJson());
    commandsStatus_->clear();

    llmEnabled_->setChecked(settings_->llmEnabled());
    llmHotkey_->setKeySequence(QKeySequence(settings_->llmHotkey(),
                                             QKeySequence::PortableText));
    llmEndpoint_->setText(settings_->llmEndpoint());
    llmApiKey_->setText(settings_->llmApiKey());
    llmModel_->setText(settings_->llmModel());
    llmTimeout_->setValue(settings_->llmTimeoutSeconds());
    llmPrompt_->setPlainText(settings_->llmPrompt());
    llmStripPattern_->setText(settings_->llmStripPattern());
    llmExtraParams_->setText(settings_->llmExtraParams());

    llmTester_->cancel();
    llmTestButton_->setEnabled(true);
    llmTestOutput_->clear();
    llmStatus_->clear();
}

void SettingsWindow::browseModel() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Select whisper model"), modelPath_->text(),
        tr("Whisper models (*.bin *.gguf);;All files (*)"));
    if (!file.isEmpty())
        modelPath_->setText(file);
}

void SettingsWindow::validateCommands() {
    QString error;
    if (CommandConfig::validate(commandsEditor_->toPlainText(), &error)) {
        commandsStatus_->setStyleSheet("color: #2e7d32;");
        commandsStatus_->setText(tr("Valid."));
    } else {
        commandsStatus_->setStyleSheet("color: #c62828;");
        commandsStatus_->setText(error);
    }
}

bool SettingsWindow::llmConfigFromFields(LlmRequestConfig* cfg,
                                         QString* error) const {
    cfg->endpoint = llmEndpoint_->text().trimmed();
    if (!cfg->endpoint.isEmpty()) {
        const QUrl url(cfg->endpoint, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() ||
            (url.scheme() != QLatin1String("https") &&
             url.scheme() != QLatin1String("http"))) {
            *error = tr("The endpoint must be an http:// or https:// URL.");
            return false;
        }
    }

    const QRegularExpression re(llmStripPattern_->text());
    if (!re.isValid()) {
        *error = tr("Invalid removal regex at offset %1: %2")
                     .arg(re.patternErrorOffset())
                     .arg(re.errorString());
        return false;
    }

    QString paramsError;
    if (!parseExtraParams(llmExtraParams_->text(), &cfg->extraParams,
                          &paramsError)) {
        *error = tr("Extra request fields: %1").arg(paramsError);
        return false;
    }

    cfg->apiKey = llmApiKey_->text().trimmed();
    cfg->model = llmModel_->text().trimmed();
    cfg->promptTemplate = llmPrompt_->toPlainText();
    cfg->stripPattern = llmStripPattern_->text();
    cfg->timeoutMs = llmTimeout_->value() * 1000;
    return true;
}

void SettingsWindow::showLlmStatus(const QString& text, bool error) {
    llmStatus_->setStyleSheet(error ? "color: #c62828;" : "color: #2e7d32;");
    llmStatus_->setText(text);
}

void SettingsWindow::testLlm() {
    LlmRequestConfig cfg;
    QString error;
    if (!llmConfigFromFields(&cfg, &error)) {
        showLlmStatus(error, true);
        return;
    }
    if (cfg.endpoint.isEmpty()) {
        showLlmStatus(tr("Enter the endpoint URL first."), true);
        return;
    }
    const QString text = llmTestInput_->text().trimmed();
    if (text.isEmpty()) {
        showLlmStatus(tr("Enter a text to send."), true);
        return;
    }

    llmTestButton_->setEnabled(false);
    llmTestOutput_->clear();
    llmStatus_->setStyleSheet(QString());
    llmStatus_->setText(tr("Waiting for the answer..."));

    QElapsedTimer elapsed;
    elapsed.start();
    llmTester_->process(cfg, text, [this, elapsed](const QString& answer,
                                                   const QString& error) {
        llmTestButton_->setEnabled(true);
        if (!error.isEmpty()) {
            showLlmStatus(tr("Failed: %1").arg(error), true);
            return;
        }
        llmTestOutput_->setPlainText(answer);
        showLlmStatus(tr("Answer in %1 s.").arg(elapsed.elapsed() / 1000.0, 0, 'f', 1),
                      false);
    });
}

void SettingsWindow::apply() {
    QString error;
    if (!CommandConfig::validate(commandsEditor_->toPlainText(), &error)) {
        tabs_->setCurrentIndex(0);
        commandsStatus_->setStyleSheet("color: #c62828;");
        commandsStatus_->setText(tr("Not saved — %1").arg(error));
        return;
    }

    LlmRequestConfig llm;
    if (!llmConfigFromFields(&llm, &error)) {
        tabs_->setCurrentWidget(llmTab_);
        showLlmStatus(tr("Not saved — %1").arg(error), true);
        return;
    }

    // Every setter emits changed(), which re-syncs the mode checkboxes from
    // the stored values: read them before the first one runs.
    const bool translate = translate_->isChecked();
    const bool llmEnabled = llmEnabled_->isChecked();
    settings_->setTranslate(translate);
    settings_->setLlmEnabled(llmEnabled);
    settings_->setVadEnabled(vadEnabled_->isChecked());
    settings_->setLanguage(language_->currentData().toString());
    settings_->setHotkey(
        hotkey_->keySequence().toString(QKeySequence::PortableText));
    settings_->setTranslateHotkey(
        translateHotkey_->keySequence().toString(QKeySequence::PortableText));
    settings_->setModelPath(modelPath_->text());
    settings_->setComputeBackend(computeBackend_->currentData().toString());

    settings_->setOverlayEnabled(overlayEnabled_->isChecked());
    settings_->setReminderBeepEnabled(reminderBeep_->isChecked());
    settings_->setLoggingEnabled(logEnabled_->isChecked());
    settings_->setClipboardRestoreDelayMs(clipboardDelay_->value());
    settings_->setCommandDetectionEnabled(cdEnabled_->isChecked());
    settings_->setCommandDetectionIntervalMs(cdInterval_->value());
    settings_->setCommandDetectionWindowSeconds(cdWindow_->value());

    settings_->setLlmHotkey(
        llmHotkey_->keySequence().toString(QKeySequence::PortableText));
    settings_->setLlmEndpoint(llm.endpoint);
    settings_->setLlmApiKey(llm.apiKey);
    settings_->setLlmModel(llm.model);
    settings_->setLlmTimeoutSeconds(llmTimeout_->value());
    settings_->setLlmPrompt(llm.promptTemplate);
    settings_->setLlmStripPattern(llm.stripPattern);
    settings_->setLlmExtraParams(llmExtraParams_->text().trimmed());

    if (!settings_->saveCommandsJson(commandsEditor_->toPlainText(), &error)) {
        commandsStatus_->setStyleSheet("color: #c62828;");
        commandsStatus_->setText(error);
        return;
    }

    commandsStatus_->setStyleSheet("color: #2e7d32;");
    commandsStatus_->setText(tr("Saved."));
    showLlmStatus(tr("Saved."), false);
    emit settingsApplied();
}

} // namespace vt
