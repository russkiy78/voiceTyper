// Run on a separate X11 display (e.g. Xvfb) to avoid sending keys to the desktop.
// Link Qt6Widgets, X11, Xtst, HotkeyParsing, the X11 hotkey/paster and
// ClipboardPasteService, including moc for both QObject service headers.
#include "clipboard/ClipboardPasteService.h"
#include "hotkey/HotkeyService.h"
#include "core/Logging.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QTextEdit>
#include <QTimer>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <cstdio>

Q_LOGGING_CATEGORY(vtInput, "voicetyper.input")

static void waitForEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return 1;
    QTextEdit editor;
    editor.show();
    waitForEvents(50);
    XSetInputFocus(display, editor.winId(), RevertToParent, CurrentTime);

    auto* hotkey = vt::HotkeyService::create(&app);
    int activations = 0;
    QObject::connect(hotkey, &vt::HotkeyService::activated,
                     [&]() { ++activations; });
    if (!hotkey->setHotkey("Ctrl+Alt+V"))
        return 2;
    const auto key = [display](KeySym symbol, bool down) {
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, symbol), down, 0);
        XSync(display, False);
    };

    // Reproduce the old failure: paste while Alt is still physically held.
    key(XK_Alt_L, true);
    auto unsafePaster = vt::KeyboardPaster::create();
    unsafePaster->sendPaste();
    waitForEvents(100);
    key(XK_Alt_L, false);
    waitForEvents(50);
    if (activations != 1) {
        std::fprintf(stderr, "Could not reproduce paste triggering hotkey\n");
        return 3;
    }

    vt::ClipboardPasteService paste;
    paste.setRestoreDelayMs(50);
    bool completed = false;
    QObject::connect(&paste, &vt::ClipboardPasteService::pasteCompleted,
                     [&]() { completed = true; });
    app.clipboard()->setText("previous clipboard");

    // Several short takes finishing before the stop modifiers come up.
    for (int i = 0; i < 5; ++i) {
        editor.clear();
        completed = false;
        const int beforePress = activations;
        key(XK_Control_L, true);
        key(XK_Alt_L, true);
        key(XK_v, true);
        waitForEvents(25);
        const int beforeRelease = activations;
        if (beforeRelease != beforePress)
            return 10;
        // Repeat presses while held must not activate the shortcut.
        key(XK_v, true);
        key(XK_v, true);
        waitForEvents(50);
        if (activations != beforeRelease)
            return 6;
        key(XK_v, false);
        waitForEvents(25);
        const int beforePaste = activations;
        if (beforePaste != beforePress)
            return 11;
        paste.pasteText("recognized text");
        waitForEvents(200); // Longer than the original 60 ms paste delay.
        const bool waited = !completed && editor.toPlainText().isEmpty() &&
                            activations == beforePaste;
        key(XK_Alt_L, false);
        key(XK_Control_L, false);
        waitForEvents(200);
        if (!waited || !completed || activations != beforePaste + 1 ||
            editor.toPlainText() != "recognized text" ||
            app.clipboard()->text() != "previous clipboard") {
            std::fprintf(stderr, "FAIL: paste/release cycle %d\n", i);
            return 4;
        }
    }

    // Reverse release order: releasing the modifiers first still waits for V.
    const int beforeReverse = activations;
    key(XK_Control_L, true);
    key(XK_Alt_L, true);
    key(XK_v, true);
    waitForEvents(50);
    key(XK_Control_L, false);
    key(XK_Alt_L, false);
    waitForEvents(100);
    if (activations != beforeReverse)
        return 7;
    key(XK_v, false);
    waitForEvents(100);
    if (activations != beforeReverse + 1)
        return 8;

    // Changing/unregistering a hotkey cancels a pending activation.
    key(XK_Control_L, true);
    key(XK_Alt_L, true);
    key(XK_v, true);
    waitForEvents(50);
    hotkey->unregisterHotkey();
    key(XK_v, false);
    key(XK_Alt_L, false);
    key(XK_Control_L, false);
    waitForEvents(100);
    if (activations != beforeReverse + 1)
        return 9;

    // Holding V alone must also delay synthesis, avoiding a fake release of V.
    key(XK_v, true);
    const bool heldV = !unsafePaster->isReadyToPaste();
    key(XK_v, false);
    const bool releasedV = unsafePaster->isReadyToPaste();
    XCloseDisplay(display);
    if (!heldV || !releasedV)
        return 5;
    std::puts("PASS: activation on full release only; repeats ignored; paste safe");
}
