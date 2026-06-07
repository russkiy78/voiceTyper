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

} // namespace vt
