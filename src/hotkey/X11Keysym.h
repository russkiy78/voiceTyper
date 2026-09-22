#pragma once

namespace vt {

// X11 / xkb keysym for a Qt::Key value, or 0 (NoSymbol) if unsupported.
// Defined in HotkeyService_x11.cpp; the portal backend names keys by it too.
unsigned long qtKeyToKeysym(int key);

} // namespace vt
