#pragma once

#include "clipboard/KeyboardPaster.h"

#include <QObject>
#include <QString>

#include <memory>

namespace vt {

// Inserts text into the active field via the clipboard:
//   1. save the current clipboard text
//   2. put the new text on the clipboard
//   3. synthesize the paste shortcut
//   4. restore the previous clipboard after a short delay
//
// Must be used from the GUI thread (QClipboard requirement).
class ClipboardPasteService : public QObject {
    Q_OBJECT
public:
    explicit ClipboardPasteService(QObject* parent = nullptr);

    void setRestoreDelayMs(int ms) { restoreDelayMs_ = ms; }
    [[nodiscard]] bool isReady() const { return paster_ != nullptr; }

    void pasteText(const QString& text);

signals:
    void pasteFailed(const QString& reason);
    void pasteCompleted();

private:
    std::unique_ptr<KeyboardPaster> paster_;
    int restoreDelayMs_ = 600;
    // Small gap so the target app observes the new clipboard before paste fires.
    int pasteDelayMs_ = 60;
};

} // namespace vt
