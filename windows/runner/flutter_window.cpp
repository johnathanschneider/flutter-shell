#include "flutter_window.h"

#include <flutter/standard_method_codec.h>

#include <optional>

#include "app_theme.h"
#include "flutter/generated_plugin_registrant.h"

namespace {

// What Dart needs to colour the page: {"dark": bool, "contrast": the
// contrast theme's Background colour as 0xAARRGGBB, or null when no
// contrast theme is on}.
flutter::EncodableValue ThemeInfo() {
  flutter::EncodableValue contrast_color;
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
  if (contrast.dwFlags & HCF_HIGHCONTRASTON) {
    const COLORREF color = GetSysColor(COLOR_WINDOW);
    contrast_color = flutter::EncodableValue(static_cast<int64_t>(
        0xFF000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) |
        GetBValue(color)));
  }
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("dark"),
       flutter::EncodableValue(AppUsesDarkMode())},
      {flutter::EncodableValue("contrast"), contrast_color},
  });
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());

  // Dart sends the app's light/dark setting at startup ("setMode", before
  // the first frame, so before the window shows) and gets back what the
  // page needs; we push changes after that ("changed").
  theme_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "flutter_shell/theme",
          &flutter::StandardMethodCodec::GetInstance());
  theme_channel_->SetMethodCallHandler(
      [this](const auto& call, auto result) {
        if (call.method_name() != "setMode") {
          result->NotImplemented();
          return;
        }
        const auto* mode = std::get_if<std::string>(call.arguments());
        if (!mode) {
          result->Error("bad-argument", "expected system, light or dark");
          return;
        }
        g_theme_mode = *mode == "light"  ? ThemeMode::kLight
                       : *mode == "dark" ? ThemeMode::kDark
                                         : ThemeMode::kSystem;
        RefreshTheme();
        result->Success(ThemeInfo());
      });
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  theme_channel_ = nullptr;
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
    // Light/dark mode or contrast theme changed.
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      SendTheme();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

void FlutterWindow::SendTheme() {
  if (theme_channel_) {
    theme_channel_->InvokeMethod(
        "changed", std::make_unique<flutter::EncodableValue>(ThemeInfo()));
  }
}
