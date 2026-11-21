#ifndef RUNNER_TITLE_BAR_H_
#define RUNNER_TITLE_BAR_H_

#include <windows.h>

#include <optional>

// A custom-drawn title bar for a Win32 window with a stock frame.
//
// Windows normally draws the title bar ("caption") itself. This class takes
// that area over so the title bar looks and behaves the same whether or not
// the window is maximized, and re-implements what the stock title bar does:
//   * app icon (click for the system menu) and title text
//   * minimize / maximize / close buttons with hover and pressed states,
//     tooltips, and Windows 11 snap layouts
//   * colours that follow light/dark mode, the "show accent colour on title
//     bars" setting and high-contrast themes, updating live
//   * accessibility: screen readers see the title bar and its buttons
//   * right-to-left layout for right-to-left Windows display languages
//
// The owning window forwards every message to HandleMessage() first.
class TitleBar {
 public:
  TitleBar() = default;
  TitleBar(const TitleBar&) = delete;
  TitleBar& operator=(const TitleBar&) = delete;
  ~TitleBar();

  // Call once the window exists (creates the tooltips, reads settings).
  void Attach(HWND window);

  // Handles a window message if it belongs to the title bar. Returns a value
  // if the message was fully handled; std::nullopt means "carry on handling
  // it as usual" (some messages are observed and then passed on).
  std::optional<LRESULT> HandleMessage(HWND window,
                                       UINT message,
                                       WPARAM wparam,
                                       LPARAM lparam);

  // Re-reads colours and redraws, after the app's light/dark setting
  // changed (Windows' own setting changes are picked up automatically).
  void Refresh(HWND window);

  // Height in pixels of the title bar strip at the top of the client area.
  int height() const { return height_; }

 private:
  // Colours for one window state (active or inactive).
  struct Palette {
    COLORREF background;
    COLORREF text;            // title text and button glyphs
    COLORREF hover;           // minimize/maximize hovered
    COLORREF pressed;         // minimize/maximize pressed
    COLORREF hover_text;      // glyph on a hovered/pressed min/max button
    COLORREF close_hover;
    COLORREF close_pressed;
    COLORREF close_hover_text;
    // Colour at the icon end of the bar, fading into |background| towards
    // the buttons. Same as |background| when there's no tint.
    COLORREF glow;
  };

  // Re-reads everything that comes from Windows settings: colours (theme,
  // accent, high contrast) and reading direction.
  void ReadSettings();

  // Height of the coloured top border band drawn in contrast themes (0
  // otherwise); the title bar's contents sit below it.
  int FrameBand() const;

  // Layout, in client coordinates.
  RECT ButtonRect(HWND window, int button) const;
  RECT IconRect(HWND window) const;
  RECT TextRect(HWND window) const;

  LRESULT HitTest(HWND window, LPARAM lparam) const;
  // Opens the window menu (Restore, Move, Size... Close) under the icon.
  void ShowSystemMenu(HWND window) const;
  void Paint(HWND window, HDC dc) const;
  void Invalidate(HWND window) const;
  void SetButtonState(HWND window, int hovered, int pressed);
  void UpdateTooltips(HWND window) const;
  LRESULT FillTitleBarInfo(HWND window, WPARAM wparam, LPARAM lparam) const;

  int height_ = 0;      // whole strip
  int top_border_ = 0;  // resize zone at its top (0 when maximized)

  int hovered_button_ = 0;  // HTMINBUTTON, HTMAXBUTTON, HTCLOSE or 0
  int pressed_button_ = 0;
  bool active_ = true;

  bool high_contrast_ = false;
  bool right_to_left_ = false;
  Palette active_palette_ = {};
  Palette inactive_palette_ = {};

  HWND tooltip_ = nullptr;

  // UI Automation root element for screen readers (created on first
  // request; see uia_title_bar.h).
  struct IRawElementProviderSimple* uia_root_ = nullptr;
};

#endif  // RUNNER_TITLE_BAR_H_
