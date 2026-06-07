#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;

namespace vt {

// Owns the system tray icon and its menu (toggle dictation, open settings, quit).
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(QObject* parent = nullptr);
    ~TrayController() override;

    [[nodiscard]] bool isAvailable() const {
        return QSystemTrayIcon::isSystemTrayAvailable();
    }

    void show();
    void setRecording(bool recording);
    void showMessage(const QString& title, const QString& body);

signals:
    void toggleRecordingRequested();
    void openSettingsRequested();
    void quitRequested();

private:
    QIcon makeIcon(bool recording) const;

    QSystemTrayIcon tray_;
    QMenu* menu_ = nullptr;
    QAction* toggleAction_ = nullptr;
    bool recording_ = false;
};

} // namespace vt
