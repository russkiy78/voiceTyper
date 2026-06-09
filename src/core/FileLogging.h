#pragma once

#include <QString>

namespace vt {

// Tees Qt's logging (qDebug/qCInfo/qCWarning/...) into <dir>/voicetyper.log,
// flushing after every line so the last message before a hard crash (abort /
// terminate / a faulting driver) still reaches disk. This is what makes an
// otherwise-silent GPU init crash diagnosable. The previously installed handler
// is chained, so default stderr output is preserved.
//
// Call once, early in main(), after the org/app names are set. `enabled` seeds
// the initial state from the persisted setting.
void installFileLogging(const QString& dir, bool enabled);

// Toggle file logging at runtime (the handler stays installed; when disabled it
// just stops writing and closes the file). Safe to call from the UI thread when
// the user changes the setting.
void setFileLoggingEnabled(bool enabled);

} // namespace vt
