#include "hotkey/HotkeyParsing.h"

#include <QKeyCombination>
#include <QKeySequence>

namespace vt {

ParsedHotkey parseHotkey(const QString& sequence) {
    ParsedHotkey out;
    if (sequence.trimmed().isEmpty())
        return out;

    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.count() < 1)
        return out;

    const QKeyCombination combo = seq[0];
    out.modifiers = combo.keyboardModifiers();
    out.key = static_cast<int>(combo.key());
    out.valid = out.key != 0 && out.key != Qt::Key_unknown;
    return out;
}

} // namespace vt
