#pragma once

class QWidget;

namespace vt::mac {

// Qt::Tool windows on macOS are backed by an NSPanel, which defaults
// hidesOnDeactivate to YES: the OS hides the panel the instant voiceTyper
// itself stops being the frontmost app. That defeats an always-on-top
// overlay meant to stay visible while the user dictates into a *different*
// app, and isn't reachable through Qt's cross-platform API — hence this
// tiny AppKit shim. Safe to call as soon as the widget exists; it forces
// native window creation if that hasn't happened yet.
void preventPanelHideOnDeactivate(QWidget* widget);

// Recording is often started while some other app's text field has focus —
// that's the whole point of a dictation hotkey. But showing the overlay
// panel can still shift macOS's notion of the active app away from it, so by
// the time the paste keystroke fires after transcription, it lands nowhere
// useful. rememberFrontmostApp() snapshots whichever app is frontmost right
// now (call it at the start of recording); restoreFrontmostApp() reactivates
// that app (call it right before synthesizing the paste keystroke) so paste
// reliably lands in the field the user was dictating into.
void rememberFrontmostApp();
void restoreFrontmostApp();

} // namespace vt::mac
