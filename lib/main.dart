import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'theme_setting.dart';

// Talks to the native side (windows/runner/flutter_window.cpp). We send the
// light/dark setting ("setMode"); it answers, and later pushes changes
// ("changed"), with {"dark": bool, "contrast": 0xAARRGGBB or null}.
const _theme = MethodChannel('flutter_shell/theme');

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  // Before drawing anything: the window only appears after the first frame,
  // so the title bar is already in the right mode when it does.
  Object? info;
  try {
    info = await _theme.invokeMethod<Object?>('setMode', themeSetting.name);
  } on MissingPluginException {
    // No native side for this platform (yet): plain light page.
  }
  runApp(Page(initialTheme: info));
}

/// A blank page: soft light grey in light mode, soft dark grey in dark mode,
/// or the contrast theme's Background colour when a Windows contrast theme
/// is on. Light vs dark comes from [themeSetting] (lib/theme_setting.dart),
/// applied by the native side so the page always matches the title bar.
class Page extends StatefulWidget {
  const Page({super.key, this.initialTheme});

  /// The native side's first answer (see [_theme]).
  final Object? initialTheme;

  @override
  State<Page> createState() => _PageState();
}

class _PageState extends State<Page> {
  // Moderate shades, not pure white/black: easier on the eyes.
  static const _light = Color(0xFFEBEBEB);
  static const _dark = Color(0xFF2B2B2B);

  bool _isDark = false;
  // The contrast theme's colour, or null when no contrast theme is on.
  Color? _contrastColor;

  @override
  void initState() {
    super.initState();
    _read(widget.initialTheme);
    // Changes pushed by the native side when the system theme changes.
    _theme.setMethodCallHandler((call) async {
      if (call.method == 'changed' && mounted) {
        setState(() => _read(call.arguments));
      }
    });
  }

  void _read(Object? info) {
    if (info is! Map) return;
    final contrast = info['contrast'];
    _isDark = info['dark'] == true;
    _contrastColor = contrast is int ? Color(contrast) : null;
  }

  @override
  Widget build(BuildContext context) =>
      ColoredBox(color: _contrastColor ?? (_isDark ? _dark : _light));
}
