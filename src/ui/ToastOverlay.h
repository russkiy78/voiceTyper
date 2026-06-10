#pragma once

#include <QTimer>
#include <QWidget>

namespace vt {

// Small always-on-top toast shown for a couple of seconds then auto-hidden.
// Used for brief status messages (e.g. translation toggle feedback).
// Positioned above the recording overlay slot so the two never overlap.
class ToastOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ToastOverlay(QWidget* parent = nullptr);

    void showToast(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void positionAboveOverlay();

    QString text_;
    QTimer hideTimer_;
};

} // namespace vt
