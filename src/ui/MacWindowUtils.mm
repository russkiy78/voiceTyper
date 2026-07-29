#include "ui/MacWindowUtils.h"

#include <QWidget>

#import <AppKit/AppKit.h>

namespace vt::mac {

void preventPanelHideOnDeactivate(QWidget* widget) {
    if (!widget)
        return;

    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    NSWindow* window = view.window;
    window.hidesOnDeactivate = NO;
}

namespace {
NSRunningApplication* g_frontmostApp = nil;
}

void rememberFrontmostApp() {
    g_frontmostApp = NSWorkspace.sharedWorkspace.frontmostApplication;
}

void restoreFrontmostApp() {
    if (g_frontmostApp && !g_frontmostApp.terminated)
        [g_frontmostApp activateWithOptions:0];
    g_frontmostApp = nil;
}

} // namespace vt::mac
