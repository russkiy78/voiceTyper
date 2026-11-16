#pragma once

namespace vt {

// X11 / xkb keysym for a Qt::Key value, or 0 (NoSymbol) if unsupported.
// keypad: the hotkey has Qt::KeypadModifier, so a numpad key (KP_Add, KP_1)
// is meant rather than its main-keyboard twin. Defined in
// HotkeyService_x11.cpp; the portal backend names keys by it too.
unsigned long qtKeyToKeysym(int key, bool keypad = false);

} // namespace vt
