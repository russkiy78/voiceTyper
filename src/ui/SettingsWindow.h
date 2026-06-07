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

namespace vt {

class SettingsStore;

// MVP settings dialog: ASR language, global hotkey, model path, overlay toggle,
// clipboard/detection tuning, and a JSON editor for the command config.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    explicit SettingsWindow(SettingsStore* settings, QWidget* parent = nullptr);

signals:
    // Emitted after settings are successfully saved so the app can reload them.
    void settingsApplied();

private slots:
    void browseModel();
    void validateCommands();
    void apply();

private:
    void buildUi();
    void loadFromSettings();

    SettingsStore* settings_ = nullptr;

    QComboBox* language_ = nullptr;
    QCheckBox* translate_ = nullptr;
    QKeySequenceEdit* hotkey_ = nullptr;
    QLineEdit* modelPath_ = nullptr;
    QComboBox* computeBackend_ = nullptr;
    QCheckBox* overlayEnabled_ = nullptr;
    QSpinBox* clipboardDelay_ = nullptr;
    QCheckBox* cdEnabled_ = nullptr;
    QSpinBox* cdInterval_ = nullptr;
    QDoubleSpinBox* cdWindow_ = nullptr;
    QPlainTextEdit* commandsEditor_ = nullptr;
    QLabel* commandsStatus_ = nullptr;
};

} // namespace vt
