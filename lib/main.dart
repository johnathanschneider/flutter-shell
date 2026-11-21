import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const Page());
}

/// A blank page: white, or the contrast theme's Background colour when a
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
  static const _white = Color(0xFFFFFFFF);

  Color _color = _white;

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
    final color = value is int ? Color(value) : _white;
    if (mounted && color != _color) setState(() => _color = color);
  }

  @override
  Widget build(BuildContext context) => ColoredBox(color: _color);
}
