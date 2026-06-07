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
        const bool ok = self->paster_->sendPaste();
        if (!ok)
            emit self->pasteFailed(tr("Failed to synthesize paste keystroke."));

        // 4. Restore the previous clipboard after the configured delay.
        const QString prev = previous;
        QTimer::singleShot(self->restoreDelayMs_, self, [self, prev, ok]() {
            if (!self)
                return;
            QGuiApplication::clipboard()->setText(prev);
            if (ok)
                emit self->pasteCompleted();
        });
    });
}

} // namespace vt
