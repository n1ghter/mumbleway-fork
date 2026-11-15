import 'package:flutter_test/flutter_test.dart';
import 'package:mumbleway/state/app_state.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() => SharedPreferences.setMockInitialValues({}));

  Future<bool> granted() async => true;
  Future<bool> refused() async => false;

  test('launch asks for the overlay permission only once', () async {
    final prefs = await SharedPreferences.getInstance();
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, refused), isTrue);
    // The rider backed out of the settings page without granting it. Every
    // later launch must leave them in the app.
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, refused), isFalse);
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, refused), isFalse);
  });

  test('the record of asking survives a restart', () async {
    final prefs = await SharedPreferences.getInstance();
    await AppState.mayAskForOverlayAtLaunch(prefs, refused);
    final values = prefs.getKeys().map((k) => MapEntry(k, prefs.get(k)!));
    SharedPreferences.setMockInitialValues(Map.fromEntries(values));
    final again = await SharedPreferences.getInstance();
    expect(await AppState.mayAskForOverlayAtLaunch(again, refused), isFalse);
  });

  test('a granted permission always restores the window', () async {
    final prefs = await SharedPreferences.getInstance();
    await AppState.mayAskForOverlayAtLaunch(prefs, refused);
    // Granted later from system settings: nothing to ask, so go ahead.
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, granted), isTrue);
  });

  test('granted at first launch spends no ask', () async {
    final prefs = await SharedPreferences.getInstance();
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, granted), isTrue);
    // Revoked afterwards: the one ask is still available.
    expect(await AppState.mayAskForOverlayAtLaunch(prefs, refused), isTrue);
  });
}
