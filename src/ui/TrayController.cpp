#include "ui/TrayController.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

namespace vt {

TrayController::TrayController(QObject* parent) : QObject(parent) {
    menu_ = new QMenu();

    QAction* version = menu_->addAction(
        QStringLiteral("voiceTyper v") + QApplication::applicationVersion());
    version->setEnabled(false);

    menu_->addSeparator();

    toggleAction_ = menu_->addAction(tr("Start dictation"));
    connect(toggleAction_, &QAction::triggered, this,
            &TrayController::toggleRecordingRequested);

    cancelAction_ = menu_->addAction(tr("Cancel dictation"));
    cancelAction_->setEnabled(false);
    connect(cancelAction_, &QAction::triggered, this,
            &TrayController::cancelRequested);

    menu_->addSeparator();

    QAction* settings = menu_->addAction(tr("Settings..."));
    connect(settings, &QAction::triggered, this,
            &TrayController::openSettingsRequested);

    menu_->addSeparator();

    QAction* quit = menu_->addAction(tr("Quit"));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);

    tray_.setContextMenu(menu_);
    tray_.setIcon(makeIcon());
    tray_.setToolTip(tr("voiceTyper — idle"));

    // A click on the icon never starts dictation: it takes focus from the field
    // the text is meant for, so the paste would land elsewhere. Windows opens
    // the menu only on a right click; a left click pops it up the same way Qt
    // does for a right click. Elsewhere the tray host decides what a click does.
#ifdef Q_OS_WIN
    connect(&tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    menu_->popup(QCursor::pos());
            });
#endif
}

TrayController::~TrayController() { delete menu_; }

void TrayController::show() { tray_.show(); }

void TrayController::setRecording(bool recording) {
    recording_ = recording;
    tray_.setIcon(makeIcon());
    tray_.setToolTip(recording ? tr("voiceTyper — recording") : tr("voiceTyper — idle"));
    if (toggleAction_)
        toggleAction_->setText(recording ? tr("Stop dictation")
                                         : tr("Start dictation"));
    updateCancelAction();
}

void TrayController::setProcessing(bool processing) {
    processing_ = processing;
    updateCancelAction();
}

void TrayController::updateCancelAction() {
    if (cancelAction_)
        cancelAction_->setEnabled(recording_ || processing_);
}

void TrayController::setTranslate(bool translate) {
    translate_ = translate;
    tray_.setIcon(makeIcon());
}

void TrayController::setLlm(bool llm) {
    llm_ = llm;
    tray_.setIcon(makeIcon());
}

void TrayController::showMessage(const QString& title, const QString& body) {
    tray_.showMessage(title, body, QSystemTrayIcon::Information, 3000);
}

QIcon TrayController::makeIcon() const {
    // Draw a simple microphone glyph so the app needs no external icon asset.
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Near-white fill + solid black outline: the highest-contrast combo
    // against light, dark, *and* colored backgrounds alike (a mid-tone fill
    // color, tried first, still washed out against similarly-toned menu
    // bars — white-on-black doesn't have that failure mode).
    const QColor body = recording_ ? QColor(231, 76, 60) : QColor(245, 245, 245);
    const QColor outlineColor(15, 15, 15);

    // The glyph is drawn twice: once oversized in the outline color, then at
    // normal size in the fill color on top.
    auto drawGlyph = [&](const QColor& color, qreal grow) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(24, 10, 16, 28).adjusted(-grow, -grow, grow, grow),
                           8 + grow, 8 + grow);

        QPen pen(color);
        pen.setWidthF(4 + 2 * grow);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(18, 16, 28, 30), 200 * 16, 140 * 16);
        p.drawLine(QPointF(32, 46), QPointF(32, 54));
        p.drawLine(QPointF(24, 54), QPointF(40, 54));
    };

    drawGlyph(outlineColor, 2);
    drawGlyph(body, 0);

    // Mode badges: "EN" (translate) on top, "AI" (LLM processing) below it.
    // Alone, a badge keeps the full height; together they split the icon.
    const bool both = translate_ && llm_;
    auto drawBadge = [&](const QRectF& badge, const QColor& color,
                         const QString& text) {
        p.setPen(QPen(outlineColor, 2));
        p.setBrush(color);
        p.drawRoundedRect(badge, 6, 6);
        p.setPen(Qt::white);
        QFont f;
        f.setPixelSize(both ? 24 : 26);
        f.setBold(true);
        p.setFont(f);
        p.drawText(badge, Qt::AlignCenter, text);
    };
    if (translate_)
        drawBadge(both ? QRectF(8, 0, 56, 31) : QRectF(8, 0, 56, 36),
                  QColor(34, 139, 34), QStringLiteral("EN"));
    if (llm_)
        drawBadge(both ? QRectF(8, 33, 56, 31) : QRectF(8, 0, 56, 36),
                  QColor(25, 118, 210), QStringLiteral("AI"));

    p.end();
    return QIcon(pm);
}

} // namespace vt
