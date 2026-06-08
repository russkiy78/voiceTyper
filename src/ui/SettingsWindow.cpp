#include "ui/SettingsWindow.h"

#include "asr/ComputeBackends.h"
#include "commands/CommandConfig.h"
#include "settings/SettingsStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
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
#include <QSpinBox>
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
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("voiceTyper — Settings"));
    setMinimumSize(560, 640);
    buildUi();
    loadFromSettings();
}

void SettingsWindow::buildUi() {
    auto* root = new QVBoxLayout(this);

    // --- General -------------------------------------------------------
    auto* form = new QFormLayout();

    language_ = new QComboBox(this);
    for (const Lang& l : kLanguages)
        language_->addItem(tr(l.name), QString::fromLatin1(l.code));
    form->addRow(tr("Recognition language:"), language_);

    translate_ = new QCheckBox(tr("Translate to English"), this);
    form->addRow(QString(), translate_);

    hotkey_ = new QKeySequenceEdit(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    hotkey_->setMaximumSequenceLength(1);
#endif
    form->addRow(tr("Global hotkey:"), hotkey_);

    // Model path + browse button.
    modelPath_ = new QLineEdit(this);
    auto* browse = new QPushButton(tr("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, &SettingsWindow::browseModel);
    auto* modelRow = new QHBoxLayout();
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->addWidget(modelPath_, 1);
    modelRow->addWidget(browse);

    // Subtle link that opens a small dialog with direct model download links.
    auto* modelLink =
        new QLabel(tr("<a href=\"#\">Download models</a>"), this);
    modelLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    modelLink->setStyleSheet(QStringLiteral("font-size: 11px;"));
    connect(modelLink, &QLabel::linkActivated, this,
            &SettingsWindow::openModelDownloadsDialog);

    // Stack the path row and the link in one form cell so the link sits right
    // beneath the field (~2px) instead of a full form-row gap away.
    auto* modelCol = new QVBoxLayout();
    modelCol->setContentsMargins(0, 0, 0, 0);
    modelCol->setSpacing(2);
    modelCol->addLayout(modelRow);
    modelCol->addWidget(modelLink);
    auto* modelRowWidget = new QWidget(this);
    modelRowWidget->setLayout(modelCol);
    form->addRow(tr("Whisper model:"), modelRowWidget);

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

    clipboardDelay_ = new QSpinBox(this);
    clipboardDelay_->setRange(0, 5000);
    clipboardDelay_->setSuffix(tr(" ms"));
    clipboardDelay_->setSingleStep(50);
    form->addRow(tr("Clipboard restore delay:"), clipboardDelay_);

    root->addLayout(form);

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

    root->addWidget(detectGroup);

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

    root->addWidget(cmdGroup, 1);

    // --- Dialog buttons -----------------------------------------------
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsWindow::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    root->addWidget(buttons);
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
    hotkey_->setKeySequence(QKeySequence(settings_->hotkey(),
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
    clipboardDelay_->setValue(settings_->clipboardRestoreDelayMs());

    cdEnabled_->setChecked(settings_->commandDetectionEnabled());
    cdInterval_->setValue(settings_->commandDetectionIntervalMs());
    cdWindow_->setValue(settings_->commandDetectionWindowSeconds());

    commandsEditor_->setPlainText(settings_->loadCommandsJson());
    commandsStatus_->clear();
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

void SettingsWindow::apply() {
    QString error;
    if (!CommandConfig::validate(commandsEditor_->toPlainText(), &error)) {
        commandsStatus_->setStyleSheet("color: #c62828;");
        commandsStatus_->setText(tr("Not saved — %1").arg(error));
        return;
    }

    settings_->setLanguage(language_->currentData().toString());
    settings_->setTranslate(translate_->isChecked());
    settings_->setHotkey(
        hotkey_->keySequence().toString(QKeySequence::PortableText));
    settings_->setModelPath(modelPath_->text());
    settings_->setComputeBackend(computeBackend_->currentData().toString());

    settings_->setOverlayEnabled(overlayEnabled_->isChecked());
    settings_->setClipboardRestoreDelayMs(clipboardDelay_->value());
    settings_->setCommandDetectionEnabled(cdEnabled_->isChecked());
    settings_->setCommandDetectionIntervalMs(cdInterval_->value());
    settings_->setCommandDetectionWindowSeconds(cdWindow_->value());

    if (!settings_->saveCommandsJson(commandsEditor_->toPlainText(), &error)) {
        commandsStatus_->setStyleSheet("color: #c62828;");
        commandsStatus_->setText(error);
        return;
    }

    commandsStatus_->setStyleSheet("color: #2e7d32;");
    commandsStatus_->setText(tr("Saved."));
    emit settingsApplied();
}

} // namespace vt
