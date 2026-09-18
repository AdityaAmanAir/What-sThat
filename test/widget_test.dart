import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:whats_that/main.dart';

void main() {
  testWidgets('shows the send screen', (tester) async {
    SharedPreferences.setMockInitialValues({});
    final preferences = await SharedPreferences.getInstance();

    await tester.pumpWidget(WhatsThatApp(preferences: preferences));

    expect(find.text('Message Sender'), findsOneWidget);
    expect(find.text('Send'), findsOneWidget);
  });
}
