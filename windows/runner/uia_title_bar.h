#ifndef RUNNER_UIA_TITLE_BAR_H_
#define RUNNER_UIA_TITLE_BAR_H_

#include <windows.h>

#include <uiautomation.h>

// UI Automation support for the custom title bar.
//
// Windows' accessibility layer (UI Automation, used by Narrator and other
// screen readers) only finds title bar buttons in a window's *non-client*
// area. Our title bar is drawn in the client area, so we describe it
// ourselves: a "title bar" element containing Minimize, Maximize/Restore and
// Close buttons that screen readers can find, read and press.
//
// The elements get their positions by asking the window, through the same
// messages Windows uses (WM_NCHITTEST, WM_GETTITLEBARINFOEX), so the layout
// lives in one place (TitleBar).
//
// Returns the root element for |window| with a reference count of 1. Hand it
// out from WM_GETOBJECT with UiaReturnRawElementProvider.
IRawElementProviderSimple* CreateTitleBarAccessibility(HWND window);

#endif  // RUNNER_UIA_TITLE_BAR_H_
