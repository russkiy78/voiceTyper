#pragma once

#include <QLoggingCategory>

// Central logging categories. Enable verbose output at runtime with e.g.:
//   QT_LOGGING_RULES="voicetyper.*=true"
Q_DECLARE_LOGGING_CATEGORY(vtApp)
Q_DECLARE_LOGGING_CATEGORY(vtAudio)
Q_DECLARE_LOGGING_CATEGORY(vtAsr)
Q_DECLARE_LOGGING_CATEGORY(vtCmd)
Q_DECLARE_LOGGING_CATEGORY(vtInput)
Q_DECLARE_LOGGING_CATEGORY(vtUi)
