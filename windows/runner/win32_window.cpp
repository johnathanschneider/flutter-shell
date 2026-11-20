#include "win32_window.h"

#include <dwmapi.h>
#include <flutter_windows.h>
#include <windowsx.h>

#include "resource.h"

namespace {

/// Window attribute that enables dark mode window decorations.
///
/// Redefined in case the developer's machine has a Windows SDK older than
/// version 10.0.22000.0.
/// See: https://docs.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

constexpr const wchar_t kWindowClassName[] = L"FLUTTER_RUNNER_WIN32_WINDOW";

/// Registry key for app theme preference.
///
/// A value of 0 indicates apps should use dark mode. A non-zero or missing
/// value indicates apps should use light mode.
constexpr const wchar_t kGetPreferredBrightnessRegKey[] =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t kGetPreferredBrightnessRegValue[] = L"AppsUseLightTheme";

// Custom title bar: true if Windows is set to dark mode for apps.
bool IsDarkMode() {
  DWORD light_mode = 1;
  DWORD light_mode_size = sizeof(light_mode);
  RegGetValue(HKEY_CURRENT_USER, kGetPreferredBrightnessRegKey,
              kGetPreferredBrightnessRegValue, RRF_RT_REG_DWORD, nullptr,
              &light_mode, &light_mode_size);
  return light_mode == 0;
}

// Custom title bar: true for the hit-test codes of our three buttons.
bool IsTitleBarButton(WPARAM const hit) {
  return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

// Custom title bar: creates the icon font Windows itself uses for these
// buttons. "Segoe Fluent Icons" ships with Windows 11; Windows 10 has the
// older "Segoe MDL2 Assets" with the same glyph codes.
HFONT CreateCaptionGlyphFont(HDC const dc, UINT const dpi) {
  const int height = -MulDiv(10, dpi, 96);
  for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
    HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             // Grayscale smoothing: ClearType would add
                             // coloured fringes to the icons.
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, face);
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

// The number of Win32Window objects that currently exist.
static int g_active_window_count = 0;

using EnableNonClientDpiScaling = BOOL __stdcall(HWND hwnd);

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
int Scale(int source, double scale_factor) {
  return static_cast<int>(source * scale_factor);
}

// Dynamically loads the |EnableNonClientDpiScaling| from the User32 module.
// This API is only needed for PerMonitor V1 awareness mode.
void EnableFullDpiSupportIfAvailable(HWND hwnd) {
  HMODULE user32_module = LoadLibraryA("User32.dll");
  if (!user32_module) {
    return;
  }
  auto enable_non_client_dpi_scaling =
      reinterpret_cast<EnableNonClientDpiScaling*>(
          GetProcAddress(user32_module, "EnableNonClientDpiScaling"));
  if (enable_non_client_dpi_scaling != nullptr) {
    enable_non_client_dpi_scaling(hwnd);
  }
  FreeLibrary(user32_module);
}

}  // namespace

// Manages the Win32Window's window class registration.
class WindowClassRegistrar {
 public:
  ~WindowClassRegistrar() = default;

  // Returns the singleton registrar instance.
  static WindowClassRegistrar* GetInstance() {
    if (!instance_) {
      instance_ = new WindowClassRegistrar();
    }
    return instance_;
  }

  // Returns the name of the window class, registering the class if it hasn't
  // previously been registered.
  const wchar_t* GetWindowClass();

  // Unregisters the window class. Should only be called if there are no
  // instances of the window.
  void UnregisterWindowClass();

 private:
  WindowClassRegistrar() = default;

  static WindowClassRegistrar* instance_;

  bool class_registered_ = false;
};

WindowClassRegistrar* WindowClassRegistrar::instance_ = nullptr;

const wchar_t* WindowClassRegistrar::GetWindowClass() {
  if (!class_registered_) {
    WNDCLASS window_class{};
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = 0;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hIcon =
        LoadIcon(window_class.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    window_class.hbrBackground = 0;
    window_class.lpszMenuName = nullptr;
    window_class.lpfnWndProc = Win32Window::WndProc;
    RegisterClass(&window_class);
    class_registered_ = true;
  }
  return kWindowClassName;
}

void WindowClassRegistrar::UnregisterWindowClass() {
  UnregisterClass(kWindowClassName, nullptr);
  class_registered_ = false;
}

Win32Window::Win32Window() {
  ++g_active_window_count;
}

Win32Window::~Win32Window() {
  --g_active_window_count;
  Destroy();
}

bool Win32Window::Create(const std::wstring& title,
                         const Point& origin,
                         const Size& size) {
  Destroy();

  const wchar_t* window_class =
      WindowClassRegistrar::GetInstance()->GetWindowClass();

  const POINT target_point = {static_cast<LONG>(origin.x),
                              static_cast<LONG>(origin.y)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);
  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  double scale_factor = dpi / 96.0;

  HWND window = CreateWindow(
      window_class, title.c_str(), WS_OVERLAPPEDWINDOW,
      Scale(origin.x, scale_factor), Scale(origin.y, scale_factor),
      Scale(size.width, scale_factor), Scale(size.height, scale_factor),
      nullptr, nullptr, GetModuleHandle(nullptr), this);

  if (!window) {
    return false;
  }

  UpdateTheme(window);

  // Make Windows recalculate the frame now, through our WM_NCCALCSIZE handler.
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE);

  return OnCreate();
}

bool Win32Window::Show() {
  return ShowWindow(window_handle_, SW_SHOWNORMAL);
}

// static
LRESULT CALLBACK Win32Window::WndProc(HWND const window,
                                      UINT const message,
                                      WPARAM const wparam,
                                      LPARAM const lparam) noexcept {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT*>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

    auto that = static_cast<Win32Window*>(window_struct->lpCreateParams);
    EnableFullDpiSupportIfAvailable(window);
    that->window_handle_ = window;
  } else if (Win32Window* that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

LRESULT
Win32Window::MessageHandler(HWND hwnd,
                            UINT const message,
                            WPARAM const wparam,
                            LPARAM const lparam) noexcept {
  switch (message) {
    // Custom title bar: decide where our client area (the part we own) starts.
    case WM_NCCALCSIZE:
      if (wparam == TRUE) {
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
        const LONG window_top = params->rgrc[0].top;

        // Let Windows compute the stock layout; the side and bottom borders
        // stay stock, and we replace the top below.
        LRESULT result = DefWindowProc(hwnd, message, wparam, lparam);

        const UINT dpi = GetDpiForWindow(hwnd);
        const int border = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

        // One title bar height for both states (like Firefox): the stock
        // un-maximized title bar, i.e. caption + top border.
        caption_height_ = GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + border;

        if (IsZoomed(hwnd)) {
          // When maximized, the top border hangs off-screen, so start our
          // area at the visible screen edge, not the window edge.
          params->rgrc[0].top = window_top + border;
          top_border_ = 0;
        } else {
          // Take over the whole top, including the resize border.
          params->rgrc[0].top = window_top;
          top_border_ = border;
        }
        return result;
      }
      break;

    case WM_NCHITTEST:
      return HitTestTitleBar(hwnd, lparam);

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      PaintTitleBar(hwnd, dc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    // Custom title bar: because hit-testing reports HTMINBUTTON etc., Windows
    // sends "non-client" mouse messages for our buttons. We handle them
    // ourselves; passing button presses to DefWindowProc would make Windows
    // draw old Windows-95-style buttons on top of ours.
    case WM_NCMOUSEMOVE: {
      // Ask for WM_NCMOUSELEAVE when the mouse leaves the title bar.
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd,
                               0};
      TrackMouseEvent(&track);
      const bool on_button = IsTitleBarButton(wparam);
      SetButtonState(hwnd, on_button ? static_cast<int>(wparam) : 0,
                     pressed_button_);
      if (on_button) {
        return 0;
      }
      break;
    }

    case WM_NCMOUSELEAVE:
      SetButtonState(hwnd, 0, 0);
      break;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (IsTitleBarButton(wparam)) {
        SetButtonState(hwnd, static_cast<int>(wparam),
                       static_cast<int>(wparam));
        return 0;
      }
      break;

    case WM_NCLBUTTONUP:
      if (IsTitleBarButton(wparam)) {
        // A click = pressed and released on the same button.
        const bool clicked = pressed_button_ == static_cast<int>(wparam);
        SetButtonState(hwnd, static_cast<int>(wparam), 0);
        if (clicked) {
          switch (wparam) {
            case HTMINBUTTON:
              SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
              break;
            case HTMAXBUTTON:
              SendMessage(hwnd, WM_SYSCOMMAND,
                          IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
              break;
            case HTCLOSE:
              SendMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
              break;
          }
        }
        return 0;
      }
      break;

    case WM_DESTROY:
      window_handle_ = nullptr;
      Destroy();
      if (quit_on_close_) {
        PostQuitMessage(0);
      }
      return 0;

    case WM_DPICHANGED: {
      auto newRectSize = reinterpret_cast<RECT*>(lparam);
      LONG newWidth = newRectSize->right - newRectSize->left;
      LONG newHeight = newRectSize->bottom - newRectSize->top;

      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

      return 0;
    }
    case WM_SIZE: {
      InvalidateTitleBar(hwnd);
      RECT rect = GetClientArea();
      if (child_content_ != nullptr) {
        // Size and position the child window.
        MoveWindow(child_content_, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
      }
      return 0;
    }

    case WM_ACTIVATE:
      active_ = LOWORD(wparam) != WA_INACTIVE;
      InvalidateTitleBar(hwnd);
      if (child_content_ != nullptr) {
        SetFocus(child_content_);
      }
      return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
      UpdateTheme(hwnd);
      InvalidateTitleBar(hwnd);
      return 0;
  }

  return DefWindowProc(window_handle_, message, wparam, lparam);
}

void Win32Window::Destroy() {
  OnDestroy();

  if (window_handle_) {
    DestroyWindow(window_handle_);
    window_handle_ = nullptr;
  }
  if (g_active_window_count == 0) {
    WindowClassRegistrar::GetInstance()->UnregisterWindowClass();
  }
}

Win32Window* Win32Window::GetThisFromHandle(HWND const window) noexcept {
  return reinterpret_cast<Win32Window*>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

void Win32Window::SetChildContent(HWND content) {
  child_content_ = content;
  SetParent(content, window_handle_);
  RECT frame = GetClientArea();

  MoveWindow(content, frame.left, frame.top, frame.right - frame.left,
             frame.bottom - frame.top, true);

  SetFocus(child_content_);
}

RECT Win32Window::GetClientArea() {
  RECT frame;
  GetClientRect(window_handle_, &frame);
  // Flutter's content goes below the title bar strip.
  frame.top += caption_height_;
  if (frame.top > frame.bottom) {
    frame.top = frame.bottom;
  }
  return frame;
}

RECT Win32Window::TitleBarButtonRect(HWND const window, int const button) {
  RECT client;
  GetClientRect(window, &client);
  // Stock Windows 11 caption buttons are 46 x (title bar height) at 100%.
  const int width = MulDiv(46, GetDpiForWindow(window), 96);
  // Counting from the right edge: close, maximize, minimize.
  const int index = button == HTCLOSE ? 0 : button == HTMAXBUTTON ? 1 : 2;
  RECT rect = {client.right - (index + 1) * width, 0,
               client.right - index * width, caption_height_};
  return rect;
}

void Win32Window::InvalidateTitleBar(HWND const window) {
  RECT strip;
  GetClientRect(window, &strip);
  strip.bottom = caption_height_;
  InvalidateRect(window, &strip, FALSE);
}

void Win32Window::SetButtonState(HWND const window,
                                 int const hovered,
                                 int const pressed) {
  if (hovered == hovered_button_ && pressed == pressed_button_) {
    return;
  }
  hovered_button_ = hovered;
  pressed_button_ = pressed;
  InvalidateTitleBar(window);
}

void Win32Window::PaintTitleBar(HWND const window, HDC const dc) {
  RECT client;
  GetClientRect(window, &client);
  const int width = client.right;
  const int height = caption_height_;
  if (width <= 0 || height <= 0) {
    return;
  }

  // Draw into an off-screen bitmap first, then copy it in one go
  // (avoids flicker).
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
  HGDIOBJ old_bitmap = SelectObject(mem, bitmap);

  // Colours approximating stock Windows 11.
  const bool dark = IsDarkMode();
  const COLORREF background = dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
  const COLORREF hover = dark ? RGB(46, 46, 46) : RGB(229, 229, 229);
  const COLORREF press = dark ? RGB(41, 41, 41) : RGB(236, 236, 236);
  const COLORREF close_hover = RGB(196, 43, 28);
  const COLORREF close_press = RGB(200, 60, 49);
  const COLORREF glyph_active = dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  const COLORREF glyph_inactive = dark ? RGB(130, 130, 130) : RGB(150, 150, 150);

  RECT strip = {0, 0, width, height};
  HBRUSH background_brush = CreateSolidBrush(background);
  FillRect(mem, &strip, background_brush);
  DeleteObject(background_brush);

  HFONT font = CreateCaptionGlyphFont(mem, GetDpiForWindow(window));
  HGDIOBJ old_font = font ? SelectObject(mem, font) : nullptr;
  SetBkMode(mem, TRANSPARENT);

  for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
    RECT rect = TitleBarButtonRect(window, button);
    const bool is_close = button == HTCLOSE;
    const bool hovered = button == hovered_button_;
    const bool pressed = hovered && button == pressed_button_;

    COLORREF fill = background;
    COLORREF glyph = active_ ? glyph_active : glyph_inactive;
    if (pressed) {
      fill = is_close ? close_press : press;
    } else if (hovered) {
      fill = is_close ? close_hover : hover;
    }
    if (is_close && (hovered || pressed)) {
      glyph = RGB(255, 255, 255);
    }

    if (fill != background) {
      HBRUSH brush = CreateSolidBrush(fill);
      FillRect(mem, &rect, brush);
      DeleteObject(brush);
    }

    // Glyph codes from the Segoe icon fonts:
    // E921 minimize, E922 maximize, E923 restore, E8BB close.
    wchar_t symbol = L'\xE8BB';
    if (button == HTMINBUTTON) {
      symbol = L'\xE921';
    } else if (button == HTMAXBUTTON) {
      symbol = IsZoomed(window) ? L'\xE923' : L'\xE922';
    }
    SetTextColor(mem, glyph);
    DrawTextW(mem, &symbol, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  if (font) {
    SelectObject(mem, old_font);
    DeleteObject(font);
  }

  BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(mem);
}

LRESULT Win32Window::HitTestTitleBar(HWND const window, LPARAM const lparam) {
  // Side and bottom borders are still stock, so Windows answers for those.
  LRESULT hit = DefWindowProc(window, WM_NCHITTEST, 0, lparam);
  if (hit != HTCLIENT) {
    return hit;
  }

  POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ScreenToClient(window, &point);

  // Top resize border (only present when not maximized), with corners.
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

  // Our buttons. Reporting HTMAXBUTTON also makes Windows 11 show its
  // snap-layouts popup when hovering the maximize button.
  for (int button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE}) {
    RECT rect = TitleBarButtonRect(window, button);
    if (PtInRect(&rect, point)) {
      return button;
    }
  }

  // The rest of the strip drags the window, double-click maximizes,
  // right-click opens the system menu: all handled by Windows for HTCAPTION.
  if (point.y < caption_height_) {
    return HTCAPTION;
  }

  return HTCLIENT;
}

HWND Win32Window::GetHandle() {
  return window_handle_;
}

void Win32Window::SetQuitOnClose(bool quit_on_close) {
  quit_on_close_ = quit_on_close;
}

bool Win32Window::OnCreate() {
  // No-op; provided for subclasses.
  return true;
}

void Win32Window::OnDestroy() {
  // No-op; provided for subclasses.
}

void Win32Window::UpdateTheme(HWND const window) {
  DWORD light_mode;
  DWORD light_mode_size = sizeof(light_mode);
  LSTATUS result = RegGetValue(HKEY_CURRENT_USER, kGetPreferredBrightnessRegKey,
                               kGetPreferredBrightnessRegValue,
                               RRF_RT_REG_DWORD, nullptr, &light_mode,
                               &light_mode_size);

  if (result == ERROR_SUCCESS) {
    BOOL enable_dark_mode = light_mode == 0;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &enable_dark_mode, sizeof(enable_dark_mode));
  }
}
