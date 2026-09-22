#pragma once

#include "clipboard/KeyboardPaster.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

namespace vt {

// Paste keystroke on Wayland through the XDG RemoteDesktop portal.
//
// XTEST via Xwayland does reach native Wayland windows, but Xwayland opens a
// new portal session for each paste and GNOME asks the user to allow every
// one, stealing focus from the target field. Our own keyboard-only session is
// allowed once: with "remember" ticked the portal returns a restore token, so
// later sessions (and later runs) start silently. With a token the session is
// opened per paste and closed shortly after, since GNOME shows a sharing
// indicator while one is open; without a token it stays open so the question
// isn't asked again.
class PortalKeyboardPaster final : public QObject, public KeyboardPaster {
    Q_OBJECT
public:
    PortalKeyboardPaster();
    ~PortalKeyboardPaster() override;

    void prepare() override;
    bool isReadyToPaste() const override;
    bool sendPaste() override;

private slots:
    void onSessionClosed(const QVariantMap& details);

private:
    enum class State { Idle, Starting, Ready, Failed };

    void selectDevices();
    void start();
    void fail(const QString& reason);
    void close();
    bool sendKey(int evdevCode, bool pressed);

    State state_ = State::Idle;
    QString session_;
    bool persistent_ = false; // the portal handed out a restore token
    QTimer idleClose_;
};

} // namespace vt
