import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  private var callPanel: FloatingPanel?
  private var cloud: CloudStore?
  private var logSink: EngineLogSink?
  private var identity: DeviceIdentity?

  /// Held for exactly as long as there is a conversation.
  ///
  /// The same bargain the phones make, in the form macOS offers. A Mac decides
  /// on its own that an app which is not in front and not visibly busy can be
  /// slowed down (App Nap), and that a machine nobody has touched can go to
  /// sleep — and either one cuts a call that was working. `beginActivity` is
  /// how an app says otherwise.
  ///
  /// The point is the *scope*. An assertion taken at launch and held for the
  /// life of the process would stop a laptop ever idling while MumbleWay sat
  /// open in a background tab, which is the desktop version of the wake lock
  /// this app used to hold on Android from the moment it was first opened.
  /// Held per call, a Mac with nothing connected sleeps exactly as it would
  /// with the app closed.
  private var callActivity: NSObjectProtocol?
  private var powerChannel: FlutterMethodChannel?
  private var buttonsChannel: FlutterMethodChannel?
  private var hotkeyMonitor: Any?
  private var boundKeyIds: Set<Int64> = []

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    // Set here rather than by renaming the product. macOS takes the window's
    // default title from `PRODUCT_NAME`, which is also the bundle's name, what
    // it is signed as and what every path in the build refers to — so changing
    // it there to change a title bar would rename the application to rename a
    // string. This changes only the string. The menu bar keeps the short name,
    // which is what a menu bar is for.
    self.title = "MumbleWay - Voice for bikers"

    RegisterGeneratedPlugins(registry: flutterViewController)
    registerOverlayChannel(with: flutterViewController.engine.binaryMessenger)
    registerPowerChannel(with: flutterViewController.engine.binaryMessenger)
    registerButtonsChannel(with: flutterViewController.engine.binaryMessenger)
    cloud = CloudStore(messenger: flutterViewController.engine.binaryMessenger)
    logSink = EngineLogSink(messenger: flutterViewController.engine.binaryMessenger)
    identity = DeviceIdentity(messenger: flutterViewController.engine.binaryMessenger)

    super.awakeFromNib()
  }

  override func close() {
    stopHotkeyMonitor()
    super.close()
  }

  deinit {
    if let monitor = hotkeyMonitor {
      NSEvent.removeMonitor(monitor)
      hotkeyMonitor = nil
    }
  }

  private func registerOverlayChannel(with messenger: FlutterBinaryMessenger) {
    let channel = FlutterMethodChannel(
      name: "mumbleway/overlay", binaryMessenger: messenger)
    let panel = FloatingPanel(channel: channel)
    callPanel = panel

    channel.setMethodCallHandler { call, result in
      switch call.method {
      case "hasPermission", "requestPermission":
        // A floating panel needs no entitlement; there is nothing to ask for.
        result(true)

      case "show":
        panel.show()
        result(true)

      case "hide":
        panel.hide()
        result(true)

      case "phrases", "setPhrases":
        panel.setPhrases(call.arguments as? [String: String] ?? [:])
        result(true)

      case "avatars":
        panel.setAvatars(call.arguments as? [String: Any] ?? [:])
        result(true)

      case "update":
        guard let arguments = call.arguments as? [String: Any] else {
          result(false)
          return
        }
        panel.update(
          names: arguments["names"] as? [String] ?? [],
          transmitting: arguments["transmitting"] as? Bool ?? false,
          connected: arguments["connected"] as? Bool ?? false,
          muted: arguments["muted"] as? Bool ?? false,
          deafened: arguments["deafened"] as? Bool ?? false,
          level: arguments["level"] as? Double ?? 0,
          threshold: arguments["threshold"] as? Double ?? 0,
          noiseFloor: arguments["noiseFloor"] as? Double ?? 0,
          speaking: arguments["speaking"] as? Bool ?? false)
        result(true)

      default:
        result(FlutterMethodNotImplemented)
      }
    }
  }

  /// Kept apart from the overlay channel, which is optional UI while the
  /// power assertion applies whenever a call is active on any platform.
  private func registerPowerChannel(with messenger: FlutterBinaryMessenger) {
    let channel = FlutterMethodChannel(
      name: "mumbleway/power", binaryMessenger: messenger)
    channel.setMethodCallHandler { [weak self] call, result in
      switch call.method {
      case "callActive":
        self?.setCallActive(call.arguments as? Bool ?? false)
        result(true)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
    powerChannel = channel
  }

  private func registerButtonsChannel(with messenger: FlutterBinaryMessenger) {
    let channel = FlutterMethodChannel(
      name: "mumbleway/buttons", binaryMessenger: messenger)
    channel.setMethodCallHandler { [weak self] call, result in
      guard let self else {
        result(nil)
        return
      }
      switch call.method {
      case "setGlobalHotkeys":
        let args = call.arguments as? [String: Any]
        let numbers = args?["keyIds"] as? [NSNumber] ?? []
        self.boundKeyIds = Set(numbers.map { $0.int64Value })
        self.updateHotkeyMonitor()
        result(true)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
    buttonsChannel = channel
  }

  private func updateHotkeyMonitor() {
    guard !boundKeyIds.isEmpty else {
      stopHotkeyMonitor()
      return
    }
    guard hotkeyMonitor == nil else { return }
    hotkeyMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.keyDown, .keyUp]) {
      [weak self] event in
      self?.handleGlobalKeyEvent(event)
    }
  }

  private func stopHotkeyMonitor() {
    if let monitor = hotkeyMonitor {
      NSEvent.removeMonitor(monitor)
      hotkeyMonitor = nil
    }
  }

  private func handleGlobalKeyEvent(_ event: NSEvent) {
    // Only forward when the app itself is not active/key; focused key events
    // already arrive through Flutter's HardwareKeyboard.
    guard !NSApplication.shared.isActive else { return }
    guard event.type == .keyDown || event.type == .keyUp else { return }
    if event.type == .keyDown && event.isARepeat { return }

    guard let keyId = resolveFlutterKeyId(for: event) else { return }
    let pressed = event.type == .keyDown
    DispatchQueue.main.async { [weak self] in
      self?.buttonsChannel?.invokeMethod(
        "globalHotkey",
        arguments: ["keyId": keyId, "pressed": pressed])
    }
  }

  private func resolveFlutterKeyId(for event: NSEvent) -> Int64? {
    if let mapped = Self.macKeyCodeToLogicalKeyId[event.keyCode],
      boundKeyIds.contains(mapped)
    {
      return mapped
    }

    if let chars = event.charactersIgnoringModifiers,
      chars.unicodeScalars.count == 1,
      let scalar = chars.unicodeScalars.first?.value
    {
      let isControl = scalar <= 0x1f || (scalar >= 0x7f && scalar <= 0x9f)
      let isFunctionPua = scalar >= 0xf700 && scalar <= 0xf8ff
      if !isControl && !isFunctionPua {
        let normalized: UInt32
        if scalar >= 0x41 && scalar <= 0x5a {
          normalized = scalar - 0x41 + 0x61
        } else if scalar >= 0xc0 && scalar <= 0xde && scalar != 0xd7 {
          normalized = scalar - 0xc0 + 0xe0
        } else {
          normalized = scalar
        }
        let candidate = Int64(normalized)
        if boundKeyIds.contains(candidate) {
          return candidate
        }
      }
    }

    // Fallback to US QWERTY logical key IDs when a non-Latin input source
    // (such as Russian) is active while a Latin binding is held.
    if let ascii = Self.macKeyCodeToAsciiLogicalKeyId[event.keyCode],
      boundKeyIds.contains(ascii)
    {
      return ascii
    }

    return nil
  }

  /// Mirrors Flutter's `kMacOsToLogicalKey` (plus Space for modifier-safe
  /// matching) from Carbon virtual key codes to `LogicalKeyboardKey.keyId`.
  private static let macKeyCodeToLogicalKeyId: [UInt16: Int64] = [
    36: 0x0010000000d,   // enter
    48: 0x00100000009,   // tab
    49: 0x00000000020,   // space
    51: 0x00100000008,   // backspace
    53: 0x0010000001b,   // escape
    57: 0x00100000104,   // capsLock
    64: 0x00100000811,   // f17
    65: 0x0020000022e,   // numpadDecimal
    67: 0x0020000022a,   // numpadMultiply
    69: 0x0020000022b,   // numpadAdd
    71: 0x0010000010a,   // numLock
    72: 0x00100000a10,   // audioVolumeUp
    73: 0x00100000a0f,   // audioVolumeDown
    74: 0x00100000a11,   // audioVolumeMute
    75: 0x0020000022f,   // numpadDivide
    76: 0x0020000020d,   // numpadEnter
    78: 0x0020000022d,   // numpadSubtract
    79: 0x00100000812,   // f18
    80: 0x00100000813,   // f19
    81: 0x0020000023d,   // numpadEqual
    82: 0x00200000230,   // numpad0
    83: 0x00200000231,   // numpad1
    84: 0x00200000232,   // numpad2
    85: 0x00200000233,   // numpad3
    86: 0x00200000234,   // numpad4
    87: 0x00200000235,   // numpad5
    88: 0x00200000236,   // numpad6
    89: 0x00200000237,   // numpad7
    90: 0x00100000814,   // f20
    91: 0x00200000238,   // numpad8
    92: 0x00200000239,   // numpad9
    95: 0x0020000022c,   // numpadComma
    96: 0x00100000805,   // f5
    97: 0x00100000806,   // f6
    98: 0x00100000807,   // f7
    99: 0x00100000803,   // f3
    100: 0x00100000808,  // f8
    101: 0x00100000809,  // f9
    103: 0x0010000080b,  // f11
    105: 0x0010000080d,  // f13
    106: 0x00100000810,  // f16
    107: 0x0010000080e,  // f14
    109: 0x0010000080a,  // f10
    110: 0x00100000505,  // contextMenu
    111: 0x0010000080c,  // f12
    113: 0x0010000080f,  // f15
    114: 0x00100000407,  // insert
    115: 0x00100000306,  // home
    116: 0x00100000308,  // pageUp
    117: 0x0010000007f,  // delete
    118: 0x00100000804,  // f4
    119: 0x00100000305,  // end
    120: 0x00100000802,  // f2
    121: 0x00100000307,  // pageDown
    122: 0x00100000801,  // f1
    123: 0x00100000302,  // arrowLeft
    124: 0x00100000303,  // arrowRight
    125: 0x00100000301,  // arrowDown
    126: 0x00100000304,  // arrowUp
  ]

  private static let macKeyCodeToAsciiLogicalKeyId: [UInt16: Int64] = [
    0x00: 0x61, 0x01: 0x73, 0x02: 0x64, 0x03: 0x66, 0x04: 0x68, 0x05: 0x67,
    0x06: 0x7a, 0x07: 0x78, 0x08: 0x63, 0x09: 0x76, 0x0b: 0x62, 0x0c: 0x71,
    0x0d: 0x77, 0x0e: 0x65, 0x0f: 0x72, 0x10: 0x79, 0x11: 0x74, 0x12: 0x31,
    0x13: 0x32, 0x14: 0x33, 0x15: 0x34, 0x16: 0x36, 0x17: 0x35, 0x18: 0x3d,
    0x19: 0x39, 0x1a: 0x37, 0x1b: 0x2d, 0x1c: 0x38, 0x1d: 0x30, 0x1e: 0x5d,
    0x1f: 0x6f, 0x20: 0x75, 0x21: 0x5b, 0x22: 0x69, 0x23: 0x70, 0x25: 0x6c,
    0x26: 0x6a, 0x27: 0x27, 0x28: 0x6b, 0x29: 0x3b, 0x2a: 0x5c, 0x2b: 0x2c,
    0x2c: 0x2f, 0x2d: 0x6e, 0x2e: 0x6d, 0x2f: 0x2e, 0x32: 0x60,
  ]

  /// Takes or drops the activity assertion as calls come and go.
  ///
  /// `.userInitiated` covers both halves of the problem: it keeps the process
  /// out of App Nap, and it stops the machine idle-sleeping under a call
  /// nobody happens to be touching — which on a desk is the normal state of a
  /// conversation. Deliberately *not*
  /// `.userInitiatedAllowingIdleSystemSleep`, which would let the Mac sleep
  /// mid-sentence.
  ///
  /// The display is left alone. Preventing sleep is not the same as keeping a
  /// screen lit, and a voice call has nothing anybody needs to look at.
  private func setCallActive(_ active: Bool) {
    if active {
      guard callActivity == nil else { return }
      callActivity = ProcessInfo.processInfo.beginActivity(
        options: [.userInitiated],
        reason: "MumbleWay call in progress")
    } else {
      guard let activity = callActivity else { return }
      ProcessInfo.processInfo.endActivity(activity)
      callActivity = nil
    }
  }
}
