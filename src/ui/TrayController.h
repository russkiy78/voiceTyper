#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;

namespace vt {

// Owns the system tray icon and its menu (toggle and cancel dictation, open
// settings, quit).
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
    // From the end of recording until the paste; with recording, enables
    // "Cancel dictation".
    void setProcessing(bool processing);
    void setTranslate(bool translate);
    void setLlm(bool llm);
    void showMessage(const QString& title, const QString& body);

signals:
    void toggleRecordingRequested();
    void cancelRequested();
    void openSettingsRequested();
    void quitRequested();

private:
    QIcon makeIcon() const;
    void updateCancelAction();

    QSystemTrayIcon tray_;
    QMenu* menu_ = nullptr;
    QAction* toggleAction_ = nullptr;
    QAction* cancelAction_ = nullptr;
    bool recording_ = false;
    bool processing_ = false;
    bool translate_ = false;
    bool llm_ = false;
};

} // namespace vt
