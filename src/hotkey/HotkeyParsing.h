#pragma once

#include <QString>
#include <Qt>

namespace vt {

// Result of parsing a portable hotkey string ("Ctrl+Alt+Space") into a single
// key plus its modifiers. Platform services translate this into native codes.
struct ParsedHotkey {
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int key = 0; // Qt::Key value of the base key
    bool valid = false;
};

ParsedHotkey parseHotkey(const QString& sequence);

// Numpad keys a hotkey can tell apart from their main-keyboard twins: digits
// and + - * /. For these, Qt::KeypadModifier in ParsedHotkey::modifiers (the
// "Num+" prefix in portable text, e.g. "Num++") selects the numpad key.
bool isKeypadHotkeyKey(int key);

} // namespace vt
