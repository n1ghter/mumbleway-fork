import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mumbleway/services/button_controller.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel('mumbleway/buttons');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final buttons = ButtonController.instance;

  late List<MethodCall> calls;
  late DateTime now;

  List<MethodCall> hotkeySyncCalls() =>
      calls.where((c) => c.method == 'setGlobalHotkeys').toList();

  Future<void> flush() => Future<void>.delayed(Duration.zero);

  Future<void> sendGlobalHotkey(int keyId, bool pressed) async {
    final data = const StandardMethodCodec().encodeMethodCall(
      MethodCall('globalHotkey', {'keyId': keyId, 'pressed': pressed}),
    );
    await messenger.handlePlatformMessage('mumbleway/buttons', data, (_) {});
  }

  setUp(() async {
    ButtonController.globalHotkeysSupportedOverride = true;
    now = DateTime(2026, 1, 1, 12, 0, 0);
    ButtonController.clockOverride = () => now;
    calls = [];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return null;
    });

    buttons.dispose();
    buttons.cancelLearning();
    buttons.setBindings(const []);
    await flush();
    calls.clear();

    buttons.onTransmit = null;
    buttons.onToggleMute = null;
    buttons.onToggleDeafen = null;
    buttons.install();
  });

  tearDown(() async {
    buttons.dispose();
    buttons.cancelLearning();
    buttons.setBindings(const []);
    await flush();
    buttons.onTransmit = null;
    buttons.onToggleMute = null;
    buttons.onToggleDeafen = null;
    ButtonController.globalHotkeysSupportedOverride = null;
    ButtonController.clockOverride = null;
    HardwareKeyboard.instance.clearState();
    messenger.setMockMethodCallHandler(channel, null);
  });

  group('setGlobalHotkeys sync', () {
    test('pushes bound keyboard keyIds on setBindings, add, and remove', () async {
      final f9 = LogicalKeyboardKey.f9.keyId;
      final keyM = LogicalKeyboardKey.keyM.keyId;

      buttons.setBindings([
        ButtonBinding(keyId: f9, action: ButtonAction.toggleMute),
      ]);
      await flush();

      expect(hotkeySyncCalls(), hasLength(1));
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [f9]});

      buttons.addBinding(
        ButtonBinding(keyId: keyM, action: ButtonAction.toggleDeafen),
      );
      await flush();

      expect(hotkeySyncCalls(), hasLength(2));
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [f9, keyM]});

      buttons.removeBinding(f9);
      await flush();

      expect(hotkeySyncCalls(), hasLength(3));
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [keyM]});

      buttons.removeBinding(keyM);
      await flush();

      expect(hotkeySyncCalls(), hasLength(4));
      expect(hotkeySyncCalls().last.arguments, {'keyIds': <int>[]});
    });

    test('excludes synthetic mobile media keyIds from desktop hotkeys', () async {
      final space = LogicalKeyboardKey.space.keyId;
      final mediaPlay = ButtonController.mediaKeyId(85);

      buttons.setBindings([
        ButtonBinding(keyId: mediaPlay, action: ButtonAction.pushToTalk),
        ButtonBinding(keyId: space, action: ButtonAction.toggleMute),
      ]);
      await flush();

      expect(hotkeySyncCalls(), hasLength(1));
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [space]});
    });

    test('pauses hotkeys while learning and restores on cancel or finish', () async {
      final f9 = LogicalKeyboardKey.f9.keyId;
      buttons.setBindings([
        ButtonBinding(keyId: f9, action: ButtonAction.toggleMute),
      ]);
      await flush();
      calls.clear();

      buttons.learnNext((_, _) {});
      await flush();
      expect(hotkeySyncCalls().last.arguments, {'keyIds': <int>[]});

      buttons.cancelLearning();
      await flush();
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [f9]});

      // Completing learning via HardwareKeyboard also restores bindings.
      calls.clear();
      buttons.learnNext((_, _) {});
      await flush();
      expect(hotkeySyncCalls().last.arguments, {'keyIds': <int>[]});

      HardwareKeyboard.instance.handleKeyEvent(
        const KeyDownEvent(
          physicalKey: PhysicalKeyboardKey.keyM,
          logicalKey: LogicalKeyboardKey.keyM,
          timeStamp: Duration.zero,
        ),
      );
      HardwareKeyboard.instance.handleKeyEvent(
        const KeyUpEvent(
          physicalKey: PhysicalKeyboardKey.keyM,
          logicalKey: LogicalKeyboardKey.keyM,
          timeStamp: Duration(milliseconds: 10),
        ),
      );
      await flush();
      expect(hotkeySyncCalls().last.arguments, {'keyIds': [f9]});
    });

    test('does not invoke setGlobalHotkeys on unsupported platforms', () async {
      ButtonController.globalHotkeysSupportedOverride = false;
      buttons.setBindings([
        ButtonBinding(
          keyId: LogicalKeyboardKey.f9.keyId,
          action: ButtonAction.toggleMute,
        ),
      ]);
      await flush();

      expect(hotkeySyncCalls(), isEmpty);
    });
  });

  group('globalHotkey channel dispatch', () {
    test('dispatches toggleMute and toggleDeafen on press only', () async {
      var mutes = 0;
      var deafens = 0;
      buttons.onToggleMute = () => mutes++;
      buttons.onToggleDeafen = () => deafens++;

      final f9 = LogicalKeyboardKey.f9.keyId;
      final keyD = LogicalKeyboardKey.keyD.keyId;
      buttons.setBindings([
        ButtonBinding(keyId: f9, action: ButtonAction.toggleMute),
        ButtonBinding(keyId: keyD, action: ButtonAction.toggleDeafen),
      ]);

      await sendGlobalHotkey(f9, true);
      await sendGlobalHotkey(f9, false);
      expect(mutes, 1);

      await sendGlobalHotkey(keyD, true);
      await sendGlobalHotkey(keyD, false);
      expect(deafens, 1);

      // Second press after window toggles again.
      now = now.add(const Duration(milliseconds: 100));
      await sendGlobalHotkey(f9, true);
      expect(mutes, 2);
    });

    test('dispatches pushToTalk press and release and toggleTransmit', () async {
      final transmits = <bool>[];
      buttons.onTransmit = transmits.add;

      final space = LogicalKeyboardKey.space.keyId;
      final keyM = LogicalKeyboardKey.keyM.keyId;
      buttons.setBindings([
        ButtonBinding(keyId: space, action: ButtonAction.pushToTalk),
        ButtonBinding(keyId: keyM, action: ButtonAction.toggleTransmit),
      ]);

      await sendGlobalHotkey(space, true);
      await sendGlobalHotkey(space, false);
      expect(transmits, [true, false]);

      transmits.clear();
      await sendGlobalHotkey(keyM, true);
      await sendGlobalHotkey(keyM, false);
      now = now.add(const Duration(milliseconds: 100));
      await sendGlobalHotkey(keyM, true);
      await sendGlobalHotkey(keyM, false);
      expect(transmits, [true, false]);
    });
  });

  group('focus deduplication', () {
    test('ignores globalHotkey within 80 ms after HardwareKeyboard press', () async {
      var mutes = 0;
      buttons.onToggleMute = () => mutes++;

      final keyM = LogicalKeyboardKey.keyM;
      buttons.setBindings([
        ButtonBinding(keyId: keyM.keyId, action: ButtonAction.toggleMute),
      ]);

      final handled = HardwareKeyboard.instance.handleKeyEvent(
        KeyDownEvent(
          physicalKey: PhysicalKeyboardKey.keyM,
          logicalKey: keyM,
          timeStamp: Duration.zero,
        ),
      );
      expect(handled, isTrue);
      expect(mutes, 1);

      now = now.add(const Duration(milliseconds: 80));
      await sendGlobalHotkey(keyM.keyId, true);
      expect(mutes, 1);

      // After the 80 ms deduplication window, a new press toggles normally.
      now = now.add(const Duration(milliseconds: 1));
      await sendGlobalHotkey(keyM.keyId, true);
      expect(mutes, 2);

      HardwareKeyboard.instance.handleKeyEvent(
        KeyUpEvent(
          physicalKey: PhysicalKeyboardKey.keyM,
          logicalKey: keyM,
          timeStamp: const Duration(milliseconds: 90),
        ),
      );
    });

    test('ignores HardwareKeyboard press within 80 ms after globalHotkey', () async {
      var mutes = 0;
      buttons.onToggleMute = () => mutes++;

      final f9 = LogicalKeyboardKey.f9;
      buttons.setBindings([
        ButtonBinding(keyId: f9.keyId, action: ButtonAction.toggleMute),
      ]);

      await sendGlobalHotkey(f9.keyId, true);
      expect(mutes, 1);

      now = now.add(const Duration(milliseconds: 20));
      final handled = HardwareKeyboard.instance.handleKeyEvent(
        KeyDownEvent(
          physicalKey: PhysicalKeyboardKey.f9,
          logicalKey: f9,
          timeStamp: const Duration(milliseconds: 20),
        ),
      );
      expect(handled, isTrue);
      expect(mutes, 1);
    });

    test('does not suppress rapid PTT release or let repeats block key-up', () async {
      final transmits = <bool>[];
      buttons.onTransmit = transmits.add;

      final space = LogicalKeyboardKey.space;
      buttons.setBindings([
        ButtonBinding(keyId: space.keyId, action: ButtonAction.pushToTalk),
      ]);

      // Press via HardwareKeyboard, repeat, then release via globalHotkey.
      HardwareKeyboard.instance.handleKeyEvent(
        KeyDownEvent(
          physicalKey: PhysicalKeyboardKey.space,
          logicalKey: space,
          timeStamp: Duration.zero,
        ),
      );
      now = now.add(const Duration(milliseconds: 30));
      HardwareKeyboard.instance.handleKeyEvent(
        KeyRepeatEvent(
          physicalKey: PhysicalKeyboardKey.space,
          logicalKey: space,
          timeStamp: const Duration(milliseconds: 30),
        ),
      );
      now = now.add(const Duration(milliseconds: 10));
      await sendGlobalHotkey(space.keyId, false);

      expect(transmits, [true, false]);
    });
  });
}
