#include "ui/OverlayWindow.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#ifdef Q_OS_MAC
#include "ui/MacWindowUtils.h"
#endif

namespace vt {

namespace {
constexpr int kWidth = 220;
constexpr int kHeight = 64;
constexpr int kMargin = 24;
} // namespace

OverlayWindow::OverlayWindow(QWidget* parent) : QWidget(parent) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                            Qt::WindowDoesNotAcceptFocus;
#ifndef Q_OS_MAC
    flags |= Qt::BypassWindowManagerHint;
#else
    // No Qt::BypassWindowManagerHint on macOS: it skips Cocoa's normal window
    // ordering, which means WindowStaysOnTopHint only actually keeps this
    // above other *voiceTyper* windows, not above whatever app the user is
    // dictating into. The whole point of this overlay is to stay visible
    // while a different app is focused, so it needs real WM-mediated
    // ordering here. Left untouched on Windows/X11, where it was presumably
    // already working as intended.
#endif
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kWidth, kHeight);

#ifdef Q_OS_MAC
    mac::preventPanelHideOnDeactivate(this);
#endif

    pulseTimer_.setInterval(500);
    connect(&pulseTimer_, &QTimer::timeout, this, [this]() {
        pulseOn_ = !pulseOn_;
        update();
    });
}

void OverlayWindow::showOverlay() {
    positionInCorner();
    pulseOn_ = true;
    pulseTimer_.start();
    show();
    raise();
}

void OverlayWindow::hideOverlay() {
    pulseTimer_.stop();
    hide();
}

void OverlayWindow::setStatus(const QString& status) {
    status_ = status;
    update();
}

void OverlayWindow::setElapsedSeconds(double seconds) {
    elapsed_ = seconds;
    update();
}

void OverlayWindow::setLevel(double level) {
    level_ = qBound(0.0, level, 1.0);
    update();
}

void OverlayWindow::positionInCorner() {
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();
    move(avail.right() - width() - kMargin, avail.bottom() - height() - kMargin);
}

void OverlayWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Rounded translucent background.
    QPainterPath bg;
    bg.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    p.fillPath(bg, QColor(28, 28, 30, 225));

    // Pulsing record dot.
    const int dotR = 7;
    const QPoint dotCenter(20, 22);
    QColor dot(231, 76, 60);
    dot.setAlpha(pulseOn_ ? 255 : 90);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(dotCenter, dotR, dotR);

    // Status text.
    p.setPen(QColor(240, 240, 240));
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() + 0.5);
    p.setFont(f);
    p.drawText(QRect(38, 8, kWidth - 100, 28), Qt::AlignVCenter | Qt::AlignLeft,
               status_);

    // Elapsed timer (mm:ss), right aligned.
    const int total = static_cast<int>(elapsed_);
    const QString time = QStringLiteral("%1:%2")
                             .arg(total / 60, 2, 10, QLatin1Char('0'))
                             .arg(total % 60, 2, 10, QLatin1Char('0'));
    QFont mono = p.font();
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(mono.pointSizeF() + 1.0);
    p.setFont(mono);
    p.drawText(QRect(kWidth - 70, 8, 58, 28), Qt::AlignVCenter | Qt::AlignRight,
               time);

    // Level meter.
    const QRect meter(20, 44, kWidth - 40, 8);
    p.setBrush(QColor(70, 70, 74));
    p.drawRoundedRect(meter, 4, 4);
    QRect fill = meter;
    fill.setWidth(static_cast<int>(meter.width() * level_));
    QColor lvl(46, 204, 113);
    if (level_ > 0.85)
        lvl = QColor(231, 76, 60);
    p.setBrush(lvl);
    p.drawRoundedRect(fill, 4, 4);
}

} // namespace vt
