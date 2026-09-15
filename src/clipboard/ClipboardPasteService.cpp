#include "clipboard/ClipboardPasteService.h"

#include "core/Logging.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>

namespace vt {

ClipboardPasteService::ClipboardPasteService(QObject* parent)
    : QObject(parent), paster_(KeyboardPaster::create()) {
    if (!paster_)
        qCWarning(vtInput) << "No keyboard paster available on this platform";
}

void ClipboardPasteService::pasteText(const QString& text) {
    if (text.isEmpty()) {
        emit pasteCompleted();
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        emit pasteFailed(tr("Clipboard unavailable."));
        return;
    }

    // 1. Save the previous clipboard text (MVP: plain text only).
    const QString previous = clipboard->text();

    // 2. Put the new text on the clipboard.
    clipboard->setText(text);

    if (!paster_) {
        emit pasteFailed(tr("Keystroke synthesis is not supported on this platform."));
        // Still restore so we do not clobber the user's clipboard permanently.
        QPointer<ClipboardPasteService> self(this);
        QTimer::singleShot(restoreDelayMs_, this, [self, previous]() {
            if (self)
                QGuiApplication::clipboard()->setText(previous);
        });
        return;
    }

    QPointer<ClipboardPasteService> self(this);

    // 3. After a short gap, synthesize paste.
    QTimer::singleShot(pasteDelayMs_, this, [self, previous]() {
        if (!self)
            return;
        self->tryPaste(previous);
    });
}

void ClipboardPasteService::tryPaste(const QString& previous) {
    if (!paster_->isReadyToPaste()) {
        QTimer::singleShot(25, this, [this, previous]() { tryPaste(previous); });
        return;
    }

    const bool ok = paster_->sendPaste();
    if (!ok)
        emit pasteFailed(tr("Failed to synthesize paste keystroke."));

    // Restore only after the actual paste, including any wait for key release.
    QTimer::singleShot(restoreDelayMs_, this, [this, previous, ok]() {
        QGuiApplication::clipboard()->setText(previous);
        if (ok)
            emit pasteCompleted();
    });
}

} // namespace vt
