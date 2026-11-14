import 'dart:ui' as ui;

import 'package:flutter_test/flutter_test.dart';
import 'package:mumbleway/screens/server_qr_screen.dart';

void main() {
  group('qrOffersSaveAs', () {
    // The Windows share sheet has no "save to a folder" target and can fail to
    // open without telling the app, so every desktop gets a save dialog beside
    // it. Phones do not: their sheet already carries "Save image".
    test('offers saving on every desktop', () {
      expect(qrOffersSaveAs('windows'), isTrue);
      expect(qrOffersSaveAs('linux'), isTrue);
      expect(qrOffersSaveAs('macos'), isTrue);
    });

    test('leaves phones to their share sheet', () {
      expect(qrOffersSaveAs('android'), isFalse);
      expect(qrOffersSaveAs('ios'), isFalse);
    });
  });

  test('the file name is safe on every file system', () {
    expect(qrFileName('Obrezki'), 'Obrezki-qr.png');
    expect(qrFileName('My: server/1?'), 'My__server_1_-qr.png');
    expect(qrFileName('Сервер'), '______-qr.png');
  });

  testWidgets('the picture is a square PNG on an opaque white field', (
    tester,
  ) async {
    await tester.runAsync(() async {
      final png = await renderQrPng('mumble://user@example.org:64738/');
      expect(png, isNotNull);
      // PNG signature, so a file saved with this is one a viewer opens.
      expect(png!.sublist(0, 8), [137, 80, 78, 71, 13, 10, 26, 10]);

      final codec = await ui.instantiateImageCodec(png);
      final image = (await codec.getNextFrame()).image;
      expect(image.width, qrImagePixels.toInt());
      expect(image.height, qrImagePixels.toInt());

      final rgba = (await image.toByteData())!;
      // Top-left pixel lies in the quiet zone: white and fully opaque, not the
      // transparent black that disappears in a dark-themed messenger.
      expect(rgba.getUint32(0), 0xFFFFFFFF);
      image.dispose();
    });
  });
}
