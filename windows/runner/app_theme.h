#ifndef RUNNER_APP_THEME_H_
#define RUNNER_APP_THEME_H_

#include <windows.h>

#include <cwchar>

// Light or dark: the app's colour mode. Everything that has a light and a
// dark look (title bar, window border, page background) asks
// AppUsesDarkMode(), so they always agree.
enum class ThemeMode {
  kSystem,  // follow Windows' "Choose your app mode" setting (live)
  kLight,   // always light
  kDark,    // always dark
};

// The setting. Change it and rebuild.
constexpr ThemeMode kThemeMode = ThemeMode::kSystem;

// True if the app should currently look dark. (Contrast themes override
// light/dark entirely; that's handled where the colours are picked.)
inline bool AppUsesDarkMode() {
  // TITLEBAR_FORCE_THEME=light/dark overrides everything, for testing.
  wchar_t forced[8] = {};
  if (GetEnvironmentVariableW(L"TITLEBAR_FORCE_THEME", forced, 8) > 0) {
    return wcscmp(forced, L"dark") == 0;
  }
  if (kThemeMode != ThemeMode::kSystem) {
    return kThemeMode == ThemeMode::kDark;
  }
  DWORD light_mode = 1;
  DWORD size = sizeof(light_mode);
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
               L"Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light_mode,
               &size);
  return light_mode == 0;
}

#endif  // RUNNER_APP_THEME_H_
