import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const Page());
}

/// A blank page: soft light grey in light mode, soft dark grey in dark mode,
/// or the contrast theme's Background colour when a Windows contrast theme
/// is on.
///
/// Light vs dark is decided by the Windows runner, from the kThemeMode flag
/// in windows/runner/app_theme.h (follow Windows, always light or always
/// dark), so the page always matches the title bar.
class Page extends StatefulWidget {
  const Page({super.key});

  @override
  State<Page> createState() => _PageState();
}

class _PageState extends State<Page> {
  // Set up by the Windows runner (windows/runner/flutter_window.cpp). It
  // sends {"dark": bool, "contrast": 0xAARRGGBB or null}.
  static const _theme = MethodChannel('flutter_shell/theme');

  // Moderate shades, not pure white/black: easier on the eyes.
  static const _light = Color(0xFFEBEBEB);
  static const _dark = Color(0xFF2B2B2B);

  bool _isDark = false;
  // The contrast theme's colour, or null when no contrast theme is on.
  Color? _contrastColor;

  @override
  void initState() {
    super.initState();
    // Changes pushed by the runner when the theme changes.
    _theme.setMethodCallHandler((call) async {
      if (call.method == 'changed') _apply(call.arguments);
    });
    // The theme right now.
    _theme.invokeMethod<Object?>('get').then(_apply);
  }

  void _apply(Object? info) {
    if (info is! Map || !mounted) return;
    final contrast = info['contrast'];
    setState(() {
      _isDark = info['dark'] == true;
      _contrastColor = contrast is int ? Color(contrast) : null;
    });
  }

  @override
  Widget build(BuildContext context) =>
      ColoredBox(color: _contrastColor ?? (_isDark ? _dark : _light));
}
