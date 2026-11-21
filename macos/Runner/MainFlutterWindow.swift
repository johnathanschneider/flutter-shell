import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  // Talks to Dart (lib/main.dart), same as the Windows side
  // (windows/runner/flutter_window.cpp): Dart sends the light/dark setting
  // ("setMode": "system", "light" or "dark") and we answer, and later push
  // changes ("changed"), with {"dark": Bool, "contrast": null}. ("contrast"
  // is Windows' contrast-theme colour; macOS has no equivalent, so it's
  // always null here.)
  private var themeChannel: FlutterMethodChannel?
  private var appearanceObservation: NSKeyValueObservation?

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    RegisterGeneratedPlugins(registry: flutterViewController)
    setUpThemeChannel(messenger: flutterViewController.engine.binaryMessenger)

    super.awakeFromNib()
  }

  private func setUpThemeChannel(messenger: FlutterBinaryMessenger) {
    let channel = FlutterMethodChannel(
      name: "flutter_shell/theme", binaryMessenger: messenger)
    channel.setMethodCallHandler { call, result in
      guard call.method == "setMode" else {
        result(FlutterMethodNotImplemented)
        return
      }
      guard let mode = call.arguments as? String else {
        result(
          FlutterError(
            code: "bad-argument", message: "expected system, light or dark",
            details: nil))
        return
      }
      // The whole app's appearance: title bar, window frame and any native
      // controls. nil means "follow the system" (and switch live with it).
      switch mode {
      case "light":
        NSApplication.shared.appearance = NSAppearance(named: .aqua)
      case "dark":
        NSApplication.shared.appearance = NSAppearance(named: .darkAqua)
      default:
        NSApplication.shared.appearance = nil
      }
      result(MainFlutterWindow.themeInfo())
    }
    themeChannel = channel

    // The system switched between light and dark (or our setting changed):
    // tell Dart, so the page follows.
    appearanceObservation = NSApplication.shared.observe(
      \.effectiveAppearance, options: [.new]
    ) { [weak self] _, _ in
      DispatchQueue.main.async {
        self?.themeChannel?.invokeMethod(
          "changed", arguments: MainFlutterWindow.themeInfo())
      }
    }
  }

  // What Dart needs to colour the page.
  private static func themeInfo() -> [String: Any] {
    let appearance = NSApplication.shared.effectiveAppearance
    let dark = appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
    return ["dark": dark, "contrast": NSNull()]
  }
}
