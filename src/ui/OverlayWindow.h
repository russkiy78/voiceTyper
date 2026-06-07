#pragma once

#include <QTimer>
#include <QWidget>

namespace vt {

// Small always-on-top, non-focus-stealing overlay shown while recording.
// Displays a pulsing record indicator, a status line, an elapsed timer and a
// live input-level meter. It deliberately never takes focus so the user's
// target text field stays active.
class OverlayWindow : public QWidget {
    Q_OBJECT
public:
    explicit OverlayWindow(QWidget* parent = nullptr);

    void showOverlay();
    void hideOverlay();

public slots:
    void setStatus(const QString& status);
    void setElapsedSeconds(double seconds);
    void setLevel(double level); // 0..1

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void positionInCorner();

    QString status_ = tr("Recording");
    double elapsed_ = 0.0;
    double level_ = 0.0;
    bool pulseOn_ = true;
    QTimer pulseTimer_;
};

} // namespace vt
