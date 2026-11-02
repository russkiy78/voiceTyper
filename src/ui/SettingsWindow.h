#pragma once

#include <QDialog>

class QComboBox;
class QKeySequenceEdit;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QTabWidget;

namespace vt {

class SettingsStore;
class LlmPostProcessor;
struct LlmRequestConfig;

// Settings dialog. "General": ASR language, global hotkeys, model path,
// overlay toggle, clipboard/detection tuning, and a JSON editor for the
// command config. "LLM": the optional chat-completions processing step.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    explicit SettingsWindow(SettingsStore* settings, QWidget* parent = nullptr);

signals:
    // Emitted after settings are successfully saved so the app can reload them.
    void settingsApplied();

protected:
    // The window is reused across openings: re-read the stored settings so
    // edits closed without Save do not linger and look applied.
    void showEvent(QShowEvent* event) override;

private slots:
    void browseModel();
    void validateCommands();
    void testLlm();
    void apply();

private:
    void buildUi();
    QWidget* buildLlmTab();
    // Reads the LLM tab into *cfg. Returns false, with *error set, when a
    // field is invalid; an empty endpoint is allowed here.
    bool llmConfigFromFields(LlmRequestConfig* cfg, QString* error) const;
    void showLlmStatus(const QString& text, bool error);
    void loadFromSettings();
    // Small modal offering direct download links for the recommended models.
    void openModelDownloadsDialog();

    SettingsStore* settings_ = nullptr;

    QComboBox* language_ = nullptr;
    QCheckBox* translate_ = nullptr;
    QCheckBox* vadEnabled_ = nullptr;
    QKeySequenceEdit* hotkey_ = nullptr;
    QKeySequenceEdit* translateHotkey_ = nullptr;
    QLineEdit* modelPath_ = nullptr;
    QComboBox* computeBackend_ = nullptr;

    QCheckBox* overlayEnabled_ = nullptr;
    QCheckBox* reminderBeep_ = nullptr;
    QCheckBox* logEnabled_ = nullptr;
    QSpinBox* clipboardDelay_ = nullptr;
    QCheckBox* cdEnabled_ = nullptr;
    QSpinBox* cdInterval_ = nullptr;
    QDoubleSpinBox* cdWindow_ = nullptr;
    QPlainTextEdit* commandsEditor_ = nullptr;
    QLabel* commandsStatus_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    QWidget* llmTab_ = nullptr;
    QCheckBox* llmEnabled_ = nullptr;
    QKeySequenceEdit* llmHotkey_ = nullptr;
    QLineEdit* llmEndpoint_ = nullptr;
    QLineEdit* llmApiKey_ = nullptr;
    QLineEdit* llmModel_ = nullptr;
    QSpinBox* llmTimeout_ = nullptr;
    QPlainTextEdit* llmPrompt_ = nullptr;
    QLineEdit* llmStripPattern_ = nullptr;
    QLineEdit* llmExtraParams_ = nullptr;
    QLineEdit* llmTestInput_ = nullptr;
    QPushButton* llmTestButton_ = nullptr;
    QPlainTextEdit* llmTestOutput_ = nullptr;
    QLabel* llmStatus_ = nullptr;
    LlmPostProcessor* llmTester_ = nullptr;
};

} // namespace vt
