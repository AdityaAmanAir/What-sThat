import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:whats_that/main.dart';

void main() {
  testWidgets('shows the send screen in portrait and landscape',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final preferences = await SharedPreferences.getInstance();

    // Portrait test
    tester.view.physicalSize = const Size(1080, 1920);
    tester.view.devicePixelRatio = 2.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(WhatsThatApp(preferences: preferences));
    expect(find.text('Message Sender'), findsOneWidget);
    expect(find.text('Send'), findsOneWidget);

    // Landscape test
    tester.view.physicalSize = const Size(1920, 1080);
    await tester.pumpAndSettle();
    expect(find.text('Message Sender'), findsOneWidget);
    expect(find.text('Send'), findsOneWidget);
  });

  testWidgets('renders video stream page in portrait and landscape',
      (tester) async {
    tester.view.physicalSize = const Size(1080, 1920);
    tester.view.devicePixelRatio = 2.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(
      const MaterialApp(
        home: VideoStreamPage(serverIp: '127.0.0.1'),
      ),
    );
    expect(find.text('Live camera stream'), findsOneWidget);
    expect(find.text('Incoming server frames'), findsOneWidget);

    // Landscape test
    tester.view.physicalSize = const Size(1920, 1080);
    await tester.pump();
    expect(find.text('Live camera stream'), findsOneWidget);
    expect(find.text('Incoming server frames'), findsOneWidget);
  });
}
