/// The app's colour mode.
enum ThemeSetting {
  /// Follow the operating system's light/dark setting (live).
  system,

  /// Always light.
  light,

  /// Always dark.
  dark,
}

/// The setting. Change it and restart the app.
///
/// It's sent to the native side at startup (see main.dart), which applies it
/// to the window's own parts too (on Windows: title bar and border). When a
/// Windows contrast theme is on, that takes over regardless.
const ThemeSetting themeSetting = ThemeSetting.system;
