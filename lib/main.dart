import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const Page());
}

/// A blank page that follows Windows: soft light grey in light mode, soft
/// dark grey in dark mode, or the contrast theme's Background colour when a
/// Windows contrast theme is on.
class Page extends StatefulWidget {
  const Page({super.key});

  @override
  State<Page> createState() => _PageState();
}

class _PageState extends State<Page> {
  // Set up by the Windows runner (windows/runner/flutter_window.cpp). It
  // sends the colour as 0xAARRGGBB, or null when no contrast theme is on.
  static const _contrast = MethodChannel('sketchpad/contrast');
  // Moderate shades, not pure white/black: easier on the eyes.
  static const _light = Color(0xFFEBEBEB);
  static const _dark = Color(0xFF2B2B2B);

  // The contrast theme's colour, or null when no contrast theme is on.
  Color? _contrastColor;

  @override
  void initState() {
    super.initState();
    // Changes pushed by the runner when the theme changes.
    _contrast.setMethodCallHandler((call) async {
      if (call.method == 'changed') _apply(call.arguments);
    });
    // The colour right now.
    _contrast.invokeMethod<int>('get').then(_apply);
  }

  void _apply(Object? value) {
    final color = value is int ? Color(value) : null;
    if (mounted && color != _contrastColor) {
      setState(() => _contrastColor = color);
    }
  }

  @override
  Widget build(BuildContext context) {
    // Light/dark mode; rebuilds by itself when the Windows setting changes.
    final dark =
        MediaQuery.platformBrightnessOf(context) == Brightness.dark;
    return ColoredBox(color: _contrastColor ?? (dark ? _dark : _light));
  }
}
