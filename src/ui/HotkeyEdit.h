#pragma once

#include "hotkey/HotkeyParsing.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QKeySequenceEdit>

namespace vt {

// QKeySequenceEdit that keeps numpad keys apart from their main-keyboard
// twins. The stock editor drops Qt::KeypadModifier, so numpad "+" is recorded
// as plain "+", and on X11 that hotkey grabs the main "=" key instead. Here the
// numpad keys the hotkey backends support are stored with the "Num+" prefix
// ("Num++"); every other key is recorded as before.
class HotkeyEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        QKeySequenceEdit::keyPressEvent(event);
        if (!event->modifiers().testFlag(Qt::KeypadModifier) ||
            !isKeypadHotkeyKey(event->key()))
            return;
        const QKeySequence recorded = keySequence();
        if (recorded.count() != 1 || recorded[0].key() != event->key())
            return;
        setKeySequence(QKeySequence(QKeyCombination(
            recorded[0].keyboardModifiers() | Qt::KeypadModifier,
            recorded[0].key())));
    }
};

} // namespace vt
