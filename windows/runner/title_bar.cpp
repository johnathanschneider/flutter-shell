#include "title_bar.h"

#include "app_theme.h"

#include "uia_title_bar.h"

#include <commctrl.h>
#include <oleacc.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Measurements of the stock Windows 11 title bar, in pixels at 100% scaling
// (96 DPI). Taken from screenshots of a stock window.
// ---------------------------------------------------------------------------
constexpr int kButtonWidth = 46;  // each caption button
constexpr int kIconLeft = 16;      // client left edge -> app icon
constexpr int kIconTextGap = 20;   // app icon -> title text
constexpr int kTextButtonGap = 8; // title text (ellipsis) -> buttons
constexpr int kGlyphSize = 10;    // caption button icon font size

// Stock colours. Windows 11 draws the *focused* title bar over "Mica" (a
// material tinted by the desktop wallpaper), which we can't reproduce here,
// so both states use the solid colour Windows uses for unfocused title bars.
constexpr COLORREF kLightBackground = RGB(0xCC, 0xCC, 0xCC);  // stock: F3F3F3
constexpr COLORREF kLightText = RGB(0x00, 0x00, 0x00);
constexpr COLORREF kLightInactiveText = RGB(0x5C, 0x5C, 0x5C);  // stock: 919191
constexpr COLORREF kDarkBackground = RGB(0x20, 0x20, 0x20);
constexpr COLORREF kDarkText = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kDarkInactiveText = RGB(0x79, 0x79, 0x79);
constexpr COLORREF kCloseRed = RGB(0xC4, 0x2B, 0x1C);

// Hovered / pressed minimize-maximize buttons are the background with a
// translucent layer of the text colour on top. Stock Windows uses about
// 4-6% hover and 2.5-4% pressed; ours are stronger to be more visible:
//   dark text (light theme):  18% hover, 12% pressed
//   light text (dark theme):  15% hover, 10% pressed
// A pressed close button is the red at 90% over the background.
constexpr double kDarkOverlayHover = 0.18;
constexpr double kDarkOverlayPressed = 0.12;
constexpr double kLightOverlayHover = 0.15;
constexpr double kLightOverlayPressed = 0.10;
constexpr double kClosePressedOpacity = 0.90;

// Dark mode accent wash: how much accent colour is mixed into the dark
// background at the icon end of the bar (focused / unfocused), and how far
// across the bar (fraction of its width) it takes to fade out completely.
constexpr double kDarkAccentGlowActive = 0.22;
constexpr double kDarkAccentGlowInactive = 0.10;
constexpr double kAccentGlowReach = 1.0;

// Where the title is centred. The buttons take more room than the icon, so
// "the middle of the bar" and "the middle of the free space between icon and
// buttons" differ. Narrow windows use the free space's middle (the title
// looks squeezed toward the buttons otherwise), wide windows the bar's
// middle, sliding smoothly between the two over this range of widths
// (logical pixels).
constexpr int kTitleCentreNarrowWidth = 400;
constexpr int kTitleCentreWideWidth = 1000;

// Tooltip texts (the stock English ones).
constexpr const wchar_t kMinimizeTip[] = L"Minimize";
constexpr const wchar_t kMaximizeTip[] = L"Maximize";
constexpr const wchar_t kRestoreTip[] = L"Restore Down";
constexpr const wchar_t kCloseTip[] = L"Close";

// Not in older SDK headers.
#ifndef WM_GETTITLEBARINFOEX
#define WM_GETTITLEBARINFOEX 0x033F
#endif

int Scale(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

bool IsCaptionButton(WPARAM hit) {
  return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

// Mixes |top| over |bottom|: opacity 0 = bottom, 1 = top.
COLORREF Blend(COLORREF top, COLORREF bottom, double opacity) {
  auto mix = [opacity](int a, int b) {
    return static_cast<BYTE>(a * opacity + b * (1.0 - opacity) + 0.5);
  };
  return RGB(mix(GetRValue(top), GetRValue(bottom)),
             mix(GetGValue(top), GetGValue(bottom)),
             mix(GetBValue(top), GetBValue(bottom)));
}

// True if black text reads better than white on |color|.
bool IsLightColor(COLORREF color) {
  const double luminance = 0.299 * GetRValue(color) +
                           0.587 * GetGValue(color) +
                           0.114 * GetBValue(color);
  return luminance > 160;
}

bool ReadRegistryDword(const wchar_t* key, const wchar_t* value, DWORD* out) {
  DWORD size = sizeof(*out);
  return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr,
                      out, &size) == ERROR_SUCCESS;
}

// Registry DWORDs that store colours as 0xAABBGGRR; COLORREF is 0x00BBGGRR.
COLORREF AbgrToColorRef(DWORD abgr) {
  return static_cast<COLORREF>(abgr & 0x00FFFFFF);
}

// True if Windows' display language reads right to left (Arabic, Hebrew...).
// The environment variable TITLEBAR_FORCE_RTL=1 forces it, for testing.
bool DisplayLanguageIsRightToLeft() {
  wchar_t forced[8] = {};
  if (GetEnvironmentVariableW(L"TITLEBAR_FORCE_RTL", forced, 8) > 0) {
    return wcscmp(forced, L"1") == 0;
  }
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT),
                       locale, LOCALE_NAME_MAX_LENGTH, 0) == 0) {
    return false;
  }
  DWORD reading_layout = 0;
  GetLocaleInfoEx(locale, LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER,
                  reinterpret_cast<LPWSTR>(&reading_layout),
                  sizeof(reading_layout) / sizeof(wchar_t));
  return reading_layout == 1;
}

// The font Windows uses for title text, at this DPI.
HFONT CreateCaptionTextFont(UINT dpi, bool bold) {
  NONCLIENTMETRICSW metrics = {sizeof(metrics)};
  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                  &metrics, 0, dpi)) {
    return nullptr;
  }
  if (bold) {
    metrics.lfCaptionFont.lfWeight = FW_BOLD;
  }
  return CreateFontIndirectW(&metrics.lfCaptionFont);
}

// The icon font Windows uses for caption buttons: "Segoe Fluent Icons" on
// Windows 11, "Segoe MDL2 Assets" (same glyph codes) on Windows 10.
HFONT CreateCaptionGlyphFont(HDC dc, UINT dpi) {
  for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
    HFONT font = CreateFontW(-Scale(kGlyphSize, dpi), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             // Grayscale smoothing: ClearType would add
                             // coloured fringes to the icons.
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    // If the font isn't installed, Windows silently substitutes another one;
    // check the face we actually got.
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t actual[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, actual);
    SelectObject(dc, old);
    if (wcscmp(actual, face) == 0) {
      return font;
    }
    DeleteObject(font);
  }
  return nullptr;
}

// The window's small icon (what the stock title bar shows).
HICON GetWindowSmallIcon(HWND window) {
  auto icon = reinterpret_cast<HICON>(
      SendMessageW(window, WM_GETICON, ICON_SMALL2, 0));
  if (!icon) {
    icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
  }
  if (!icon) {
    icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
  }
  return icon;
}

std::wstring GetTitle(HWND window) {
  std::wstring title(GetWindowTextLengthW(window) + 1, L'\0');
  title.resize(GetWindowTextW(window, title.data(),
                              static_cast<int>(title.size())));
  return title;
}

}  // namespace

TitleBar::~TitleBar() {
  if (tooltip_) {
    DestroyWindow(tooltip_);
  }
  if (uia_root_) {
    uia_root_->Release();
  }
}

void TitleBar::Attach(HWND window) {
  ReadSettings();

  // Tooltips for the three buttons. Windows doesn't send our "non-client"
  // mouse moves to the tooltip by itself, so we relay them (see
  // WM_NCMOUSEMOVE); the texts are fetched on demand (TTN_GETDISPINFO).
  INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                             WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, window, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
  for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
    TOOLINFOW tool = {sizeof(tool)};
    tool.hwnd = window;
    tool.uId = static_cast<UINT_PTR>(button);
    tool.rect = ButtonRect(window, button);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  }
}

std::optional<LRESULT> TitleBar::HandleMessage(HWND window,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
  switch (message) {
    // Where our client area (the part we draw) starts. We take over the top
    // of the window, including the stock title bar.
    case WM_NCCALCSIZE: {
      if (wparam != TRUE) {
        return std::nullopt;
      }
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
      const LONG window_top = params->rgrc[0].top;
      // Stock layout first: the side and bottom borders stay stock.
      const LRESULT result = DefWindowProcW(window, message, wparam, lparam);

      const UINT dpi = GetDpiForWindow(window);
      const int border = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      // Same height maximized or not: the stock un-maximized title bar.
      height_ = GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + border;
      if (IsZoomed(window)) {
        // Maximized windows hang their border off-screen; start at the
        // visible screen edge instead.
        params->rgrc[0].top = window_top + border;
        top_border_ = 0;
      } else {
        params->rgrc[0].top = window_top;
        top_border_ = border;
      }
      return result;
    }

    case WM_NCHITTEST:
      return HitTest(window, lparam);

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(window, &ps);
      Paint(window, dc);
      EndPaint(window, &ps);
      return 0;
    }

    // Our buttons report HTMINBUTTON etc. from hit-testing, so Windows sends
    // "non-client" mouse messages for them. We handle those ourselves:
    // DefWindowProc would draw old Windows-95-style buttons on top of ours.
    case WM_NCMOUSEMOVE: {
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                               window, 0};
      TrackMouseEvent(&track);  // ask for WM_NCMOUSELEAVE

      // Relay the move to the tooltip, in client coordinates.
      POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      POINT client = point;
      ScreenToClient(window, &client);
      MSG relay = {window, WM_MOUSEMOVE, 0, MAKELPARAM(client.x, client.y),
                   static_cast<DWORD>(GetMessageTime()), point};
      SendMessageW(tooltip_, TTM_RELAYEVENT, 0,
                   reinterpret_cast<LPARAM>(&relay));

      const bool on_button = IsCaptionButton(wparam);
      SetButtonState(window, on_button ? static_cast<int>(wparam) : 0,
                     pressed_button_);
      if (on_button) {
        return 0;
      }
      return std::nullopt;
    }

    case WM_NCMOUSELEAVE:
      SendMessageW(tooltip_, TTM_POP, 0, 0);
      SetButtonState(window, 0, 0);
      return std::nullopt;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (IsCaptionButton(wparam)) {
        SendMessageW(tooltip_, TTM_POP, 0, 0);
        SetButtonState(window, static_cast<int>(wparam),
                       static_cast<int>(wparam));
        return 0;
      }
      return std::nullopt;

    case WM_NCLBUTTONUP:
      if (IsCaptionButton(wparam)) {
        // A click = pressed and released on the same button.
        const bool clicked = pressed_button_ == static_cast<int>(wparam);
        SetButtonState(window, static_cast<int>(wparam), 0);
        if (clicked) {
          WPARAM command = SC_CLOSE;
          if (wparam == HTMINBUTTON) {
            command = SC_MINIMIZE;
          } else if (wparam == HTMAXBUTTON) {
            command = IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
          }
          SendMessageW(window, WM_SYSCOMMAND, command, 0);
        }
        return 0;
      }
      return std::nullopt;

    // Observed, then passed on.
    case WM_ACTIVATE:
      active_ = LOWORD(wparam) != WA_INACTIVE;
      Invalidate(window);
      return std::nullopt;

    case WM_SIZE:
      UpdateTooltips(window);
      Invalidate(window);
      return std::nullopt;

    // Title or icon changed: let Windows store it, then repaint ours.
    case WM_SETTEXT:
    case WM_SETICON: {
      const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
      Invalidate(window);
      return result;
    }

    // Live updates when Windows settings change: light/dark mode
    // ("ImmersiveColorSet"), accent colour, high contrast, system colours,
    // display language.
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      ReadSettings();
      UpdateTooltips(window);
      Invalidate(window);
      return std::nullopt;

    // Tooltip asking for its text.
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom != tooltip_ || header->code != TTN_GETDISPINFOW) {
        return std::nullopt;
      }
      auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
      switch (info->hdr.idFrom) {
        case HTMINBUTTON:
          info->lpszText = const_cast<wchar_t*>(kMinimizeTip);
          break;
        case HTMAXBUTTON:
          info->lpszText = const_cast<wchar_t*>(
              IsZoomed(window) ? kRestoreTip : kMaximizeTip);
          break;
        default:
          info->lpszText = const_cast<wchar_t*>(kCloseTip);
          break;
      }
      return 0;
    }

    // Accessibility: screen readers ask Windows for the title bar's parts,
    // and Windows asks the window with this message. Answer with our layout.
    case WM_GETTITLEBARINFOEX:
      return FillTitleBarInfo(window, wparam, lparam);

    // UI Automation (Narrator etc.) asking for the window's accessibility
    // tree: hand out ours, which adds the title bar and its buttons (see
    // uia_title_bar.h). Other requests (e.g. the older MSAA) go on to Windows.
    case WM_GETOBJECT:
      if (static_cast<long>(lparam) == static_cast<long>(UiaRootObjectId)) {
        if (!uia_root_) {
          uia_root_ = CreateTitleBarAccessibility(window);
        }
        return UiaReturnRawElementProvider(window, wparam, lparam, uia_root_);
      }
      return std::nullopt;

    // Window closing: tell UI Automation our elements are gone.
    case WM_DESTROY:
      if (uia_root_) {
        UiaReturnRawElementProvider(window, 0, 0, nullptr);
        UiaDisconnectProvider(uia_root_);
        uia_root_->Release();
        uia_root_ = nullptr;
      }
      return std::nullopt;
  }
  return std::nullopt;
}

void TitleBar::ReadSettings() {
  constexpr const wchar_t kDwm[] = L"Software\\Microsoft\\Windows\\DWM";

  right_to_left_ = DisplayLanguageIsRightToLeft();

  // Contrast themes: the theme's colours, the way Windows 11's own apps use
  // them (all editable in the contrast theme editor): the bar is Background,
  // unfocused text is Inactive text, hovered/pressed buttons are Selected
  // text (Highlight). Title text and icons use Button text (see Paint).
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
  high_contrast_ = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
  if (high_contrast_) {
    const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
    const COLORREF highlight_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    active_palette_ = {GetSysColor(COLOR_WINDOW),
                       GetSysColor(COLOR_WINDOWTEXT),
                       highlight, highlight, highlight_text,
                       highlight, highlight, highlight_text};
    inactive_palette_ = active_palette_;
    inactive_palette_.text = GetSysColor(COLOR_GRAYTEXT);
    active_palette_.glow = active_palette_.background;
    inactive_palette_.glow = inactive_palette_.background;
    return;
  }

  const bool dark = AppUsesDarkMode();  // see app_theme.h

  // Builds a palette for a background + text colour, using the measured
  // hover/pressed overlays.
  auto make = [](COLORREF background, COLORREF text, COLORREF strong_text) {
    const bool light_text = IsLightColor(strong_text);
    const COLORREF overlay = light_text ? RGB(255, 255, 255) : RGB(0, 0, 0);
    const double hover = light_text ? kLightOverlayHover : kDarkOverlayHover;
    const double pressed =
        light_text ? kLightOverlayPressed : kDarkOverlayPressed;
    return Palette{background,
                   text,
                   Blend(overlay, background, hover),
                   Blend(overlay, background, pressed),
                   strong_text,
                   kCloseRed,
                   Blend(kCloseRed, background, kClosePressedOpacity),
                   RGB(255, 255, 255),
                   background};
  };

  const COLORREF background = dark ? kDarkBackground : kLightBackground;
  const COLORREF text = dark ? kDarkText : kLightText;
  active_palette_ = make(background, text, text);
  inactive_palette_ =
      make(background, dark ? kDarkInactiveText : kLightInactiveText, text);

  DWORD prevalence = 0;
  DWORD accent = 0;
  const bool has_accent = ReadRegistryDword(kDwm, L"AccentColor", &accent);

  // Dark mode: a faint wash of the Windows accent colour at the icon end of
  // the bar, fading out before the buttons. Weaker when unfocused.
  if (dark && has_accent) {
    const COLORREF accent_color = AbgrToColorRef(accent);
    active_palette_.glow =
        Blend(accent_color, background, kDarkAccentGlowActive);
    inactive_palette_.glow =
        Blend(accent_color, background, kDarkAccentGlowInactive);
  }

  // "Show accent colour on title bars and window borders" (replaces the
  // above with a solid accent bar).
  if (ReadRegistryDword(kDwm, L"ColorPrevalence", &prevalence) &&
      prevalence == 1 && has_accent) {
    const COLORREF accent_color = AbgrToColorRef(accent);
    const COLORREF accent_text =
        IsLightColor(accent_color) ? RGB(0, 0, 0) : RGB(255, 255, 255);
    active_palette_ = make(accent_color, accent_text, accent_text);
    DWORD inactive_accent = 0;
    if (ReadRegistryDword(kDwm, L"AccentColorInactive", &inactive_accent)) {
      const COLORREF color = AbgrToColorRef(inactive_accent);
      const COLORREF color_text =
          IsLightColor(color) ? RGB(0, 0, 0) : RGB(255, 255, 255);
      inactive_palette_ = make(color, color_text, color_text);
    }
  }
}

int TitleBar::FrameBand() const {
  // In contrast themes Windows paints window borders in the theme's title
  // colour; we own the top border, so we paint it (only when not maximized,
  // like stock, since a maximized window's border is off-screen).
  return high_contrast_ ? top_border_ : 0;
}

RECT TitleBar::ButtonRect(HWND window, int button) const {
  RECT client;
  GetClientRect(window, &client);
  const int width = Scale(kButtonWidth, GetDpiForWindow(window));
  // Counting from the outer edge: close, maximize, minimize.
  const int index = button == HTCLOSE ? 0 : button == HTMAXBUTTON ? 1 : 2;
  RECT rect = {0, FrameBand(), 0, height_};
  if (right_to_left_) {
    rect.left = client.left + index * width;
    rect.right = rect.left + width;
  } else {
    rect.right = client.right - index * width;
    rect.left = rect.right - width;
  }
  return rect;
}

RECT TitleBar::IconRect(HWND window) const {
  RECT client;
  GetClientRect(window, &client);
  const UINT dpi = GetDpiForWindow(window);
  const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
  const int left = Scale(kIconLeft, dpi);
  const int band = FrameBand();
  const int top = band + (height_ - band - size) / 2;
  if (right_to_left_) {
    return {client.right - left - size, top, client.right - left, top + size};
  }
  return {left, top, left + size, top + size};
}

RECT TitleBar::TextRect(HWND window) const {
  const UINT dpi = GetDpiForWindow(window);
  const RECT icon = IconRect(window);
  const int gap = Scale(kIconTextGap, dpi);
  const int button_gap = Scale(kTextButtonGap, dpi);
  if (right_to_left_) {
    const RECT last = ButtonRect(window, HTMINBUTTON);
    return {last.right + button_gap, FrameBand(), icon.left - gap, height_};
  }
  const RECT first = ButtonRect(window, HTMINBUTTON);
  return {icon.right + gap, FrameBand(), first.left - button_gap, height_};
}

LRESULT TitleBar::HitTest(HWND window, LPARAM lparam) const {
  // Side and bottom borders are still stock, so Windows answers for those.
  const LRESULT hit = DefWindowProcW(window, WM_NCHITTEST, 0, lparam);
  if (hit != HTCLIENT) {
    return hit;
  }

  POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ScreenToClient(window, &point);

  // Top resize border (only when not maximized), with corners.
  if (point.y < top_border_) {
    RECT client;
    GetClientRect(window, &client);
    if (point.x < top_border_) {
      return HTTOPLEFT;
    }
    if (point.x >= client.right - top_border_) {
      return HTTOPRIGHT;
    }
    return HTTOP;
  }

  if (point.y < height_) {
    // Reporting HTMAXBUTTON also makes Windows 11 show its snap-layouts
    // popup when hovering the maximize button.
    for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
      const RECT rect = ButtonRect(window, button);
      if (PtInRect(&rect, point)) {
        return button;
      }
    }
    // Icon: click opens the system menu, double-click closes (stock). The
    // target is the whole corner, a caption button wide and the full bar
    // tall, not just the 16x16 icon - the same size as the close button.
    RECT client;
    GetClientRect(window, &client);
    const int menu_width = Scale(kButtonWidth, GetDpiForWindow(window));
    const bool in_menu_corner = right_to_left_
                                    ? point.x >= client.right - menu_width
                                    : point.x < client.left + menu_width;
    if (in_menu_corner) {
      return HTSYSMENU;
    }
    // The rest drags the window; double-click maximizes; right-click opens
    // the system menu. Windows does all of that for HTCAPTION.
    return HTCAPTION;
  }
  return HTCLIENT;
}

void TitleBar::Paint(HWND window, HDC dc) const {
  RECT client;
  GetClientRect(window, &client);
  const int width = client.right;
  if (width <= 0 || height_ <= 0) {
    return;
  }
  const UINT dpi = GetDpiForWindow(window);
  const Palette& colors = active_ ? active_palette_ : inactive_palette_;

  // Draw off-screen, then copy in one go (no flicker).
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height_);
  HGDIOBJ old_bitmap = SelectObject(mem, bitmap);
  SetBkMode(mem, TRANSPARENT);

  const RECT strip = {0, 0, width, height_};
  HBRUSH background = CreateSolidBrush(colors.background);
  FillRect(mem, &strip, background);
  DeleteObject(background);

  // Contrast themes: the top border band, matching the sides Windows draws
  // (focused: Active Title colour, unfocused: Inactive Title colour).
  if (const int band = FrameBand()) {
    HBRUSH frame = CreateSolidBrush(
        GetSysColor(active_ ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION));
    const RECT top = {0, 0, width, band};
    FillRect(mem, &top, frame);
    DeleteObject(frame);
  }

  // Accent wash: one 1-pixel column at a time, from |glow| at the icon end
  // easing smoothly into the background (mirrored for right-to-left).
  if (colors.glow != colors.background) {
    const int reach = static_cast<int>(width * kAccentGlowReach);
    SelectObject(mem, GetStockObject(DC_BRUSH));
    for (int i = 0; i < reach; i++) {
      const double t = static_cast<double>(i) / reach;  // 0 at icon end
      const double strength = 1.0 - t * t * (3.0 - 2.0 * t);  // smoothstep
      SetDCBrushColor(mem, Blend(colors.glow, colors.background, strength));
      const int x = right_to_left_ ? width - 1 - i : i;
      const RECT column = {x, 0, x + 1, height_};
      FillRect(mem, &column, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
  }

  // App icon.
  if (HICON icon = GetWindowSmallIcon(window)) {
    const RECT rect = IconRect(window);
    DrawIconEx(mem, rect.left, rect.top, icon, rect.right - rect.left,
               rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
  }

  // Title text, bold, cut off with "..." if it doesn't fit. In contrast
  // themes it's in the theme's Button text colour, matching the icons.
  if (HFONT font = CreateCaptionTextFont(dpi, /*bold=*/true)) {
    HGDIOBJ old_font = SelectObject(mem, font);
    // Letters have soft edges, so text reads lighter than the button icons in
    // the same colour. When unfocused, nudge the title toward the focused
    // text colour to make up for it: 30% (darker) on light title bars, 50%
    // (lighter) on dark ones. High-contrast themes keep their exact system
    // colours.
    COLORREF text_color = colors.text;
    if (!active_ && !high_contrast_) {
      const bool dark_bar = IsLightColor(active_palette_.text);
      text_color =
          Blend(active_palette_.text, colors.text, dark_bar ? 0.50 : 0.30);
    }
    if (high_contrast_) {
      text_color = GetSysColor(COLOR_BTNTEXT);
    }
    SetTextColor(mem, text_color);
    // Centred: in the free space between icon and buttons when the window
    // is narrow, across the whole bar when it's wide, sliding in between
    // (see kTitleCentreNarrowWidth). Never overlaps the icon or buttons;
    // cut off with "..." if it doesn't fit.
    RECT rect = TextRect(window);  // the space it's allowed in
    const std::wstring title = GetTitle(window);
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    if (right_to_left_) {
      format |= DT_RTLREADING;
    }
    RECT measured = {0, 0, 0, 0};
    DrawTextW(mem, title.c_str(), static_cast<int>(title.size()), &measured,
              DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    const int text_width = measured.right - measured.left;
    if (text_width < rect.right - rect.left) {
      const double bar_centre = width / 2.0;
      const double space_centre = (rect.left + rect.right) / 2.0;
      const double narrow = Scale(kTitleCentreNarrowWidth, dpi);
      const double wide = Scale(kTitleCentreWideWidth, dpi);
      double t = (width - narrow) / (wide - narrow);
      t = std::clamp(t, 0.0, 1.0);
      t = t * t * (3.0 - 2.0 * t);  // smoothstep: eases in and out
      const double centre = space_centre + (bar_centre - space_centre) * t;
      int left = static_cast<int>(centre - text_width / 2.0 + 0.5);
      left = std::max(left, static_cast<int>(rect.left));
      left = std::min(left, static_cast<int>(rect.right) - text_width);
      rect.left = left;
      rect.right = left + text_width;
    }
    DrawTextW(mem, title.c_str(), static_cast<int>(title.size()), &rect,
              format);
    SelectObject(mem, old_font);
    DeleteObject(font);
  }

  // Caption buttons.
  HFONT glyph_font = CreateCaptionGlyphFont(mem, dpi);
  HGDIOBJ old_font = glyph_font ? SelectObject(mem, glyph_font) : nullptr;
  for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
    RECT rect = ButtonRect(window, button);
    const bool is_close = button == HTCLOSE;
    const bool hovered = button == hovered_button_;
    const bool pressed = hovered && button == pressed_button_;

    COLORREF fill = colors.background;
    COLORREF glyph = colors.text;
    // Contrast themes, focused or not: the icons take the theme's Button
    // text colour (e.g. yellow in Night sky), like Windows 11's Settings app.
    if (high_contrast_) {
      glyph = GetSysColor(COLOR_BTNTEXT);
    }
    if (pressed) {
      fill = is_close ? colors.close_pressed : colors.pressed;
    } else if (hovered) {
      fill = is_close ? colors.close_hover : colors.hover;
    }
    if (hovered || pressed) {
      glyph = is_close ? colors.close_hover_text : colors.hover_text;
    }
    if (fill != colors.background) {
      HBRUSH brush = CreateSolidBrush(fill);
      FillRect(mem, &rect, brush);
      DeleteObject(brush);
    }

    // Segoe icon font codes: E921 minimize, E922 maximize, E923 restore,
    // E8BB close.
    wchar_t symbol = L'\xE8BB';
    if (button == HTMINBUTTON) {
      symbol = L'\xE921';
    } else if (button == HTMAXBUTTON) {
      symbol = IsZoomed(window) ? L'\xE923' : L'\xE922';
    }
    SetTextColor(mem, glyph);
    // Contrast themes: bolder icons, by drawing the glyph again shifted
    // right, down and diagonally (one pixel at 100% scale).
    const int thicken = high_contrast_ ? Scale(1, dpi) : 0;
    for (int dx = 0; dx <= thicken; dx++) {
      for (int dy = 0; dy <= thicken; dy++) {
        RECT shifted = {rect.left + dx, rect.top + dy, rect.right + dx,
                        rect.bottom + dy};
        DrawTextW(mem, &symbol, 1, &shifted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
    }
  }
  if (glyph_font) {
    SelectObject(mem, old_font);
    DeleteObject(glyph_font);
  }

  BitBlt(dc, 0, 0, width, height_, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(mem);
}

void TitleBar::Invalidate(HWND window) const {
  RECT strip;
  GetClientRect(window, &strip);
  strip.bottom = height_;
  InvalidateRect(window, &strip, FALSE);
}

void TitleBar::SetButtonState(HWND window, int hovered, int pressed) {
  if (hovered == hovered_button_ && pressed == pressed_button_) {
    return;
  }
  hovered_button_ = hovered;
  pressed_button_ = pressed;
  Invalidate(window);
}

void TitleBar::UpdateTooltips(HWND window) const {
  if (!tooltip_) {
    return;
  }
  for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
    TOOLINFOW tool = {sizeof(tool)};
    tool.hwnd = window;
    tool.uId = static_cast<UINT_PTR>(button);
    tool.rect = ButtonRect(window, button);
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0,
                 reinterpret_cast<LPARAM>(&tool));
  }
}

LRESULT TitleBar::FillTitleBarInfo(HWND window,
                                   WPARAM wparam,
                                   LPARAM lparam) const {
  // Let Windows fill in its defaults (sizes, flags), then correct the parts
  // we moved. Index meanings are fixed by Windows: 0 title bar, 2 minimize,
  // 3 maximize, 4 help, 5 close.
  const LRESULT result = DefWindowProcW(window, WM_GETTITLEBARINFOEX, wparam,
                                        lparam);
  auto* info = reinterpret_cast<TITLEBARINFOEX*>(lparam);
  if (!info || info->cbSize < sizeof(TITLEBARINFOEX)) {
    return result;
  }

  auto to_screen = [window](RECT rect) {
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
  };
  RECT strip;
  GetClientRect(window, &strip);
  strip.bottom = height_;
  info->rcTitleBar = to_screen(strip);
  info->rgstate[0] = STATE_SYSTEM_FOCUSABLE;

  const int buttons[] = {HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
  const int indexes[] = {2, 3, 5};
  for (int i = 0; i < 3; i++) {
    info->rgrect[indexes[i]] = to_screen(ButtonRect(window, buttons[i]));
    DWORD state = 0;
    if (buttons[i] == hovered_button_) {
      state |= STATE_SYSTEM_HOTTRACKED;
      if (buttons[i] == pressed_button_) {
        state |= STATE_SYSTEM_PRESSED;
      }
    }
    info->rgstate[indexes[i]] = state;
  }
  info->rgstate[4] = STATE_SYSTEM_INVISIBLE;  // no help button
  return result;
}
