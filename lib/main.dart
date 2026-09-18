import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

const _serverIpKey = 'server_ip';
const _serverPort = 5000;

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final preferences = await SharedPreferences.getInstance();
  runApp(WhatsThatApp(preferences: preferences));
}

class WhatsThatApp extends StatelessWidget {
  const WhatsThatApp({super.key, required this.preferences});

  final SharedPreferences preferences;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Message Sender',
      theme: ThemeData(colorSchemeSeed: Colors.blue, useMaterial3: true),
      home: SendMessagePage(preferences: preferences),
    );
  }
}

class SendMessagePage extends StatefulWidget {
  const SendMessagePage({super.key, required this.preferences});

  final SharedPreferences preferences;

  @override
  State<SendMessagePage> createState() => _SendMessagePageState();
}

class _SendMessagePageState extends State<SendMessagePage> {
  final _messageController = TextEditingController();
  final _receivedMessages = <String>[];
  Socket? _socket;
  StreamSubscription<String>? _messageSubscription;
  bool _isConnecting = false;

  String get _serverIp => widget.preferences.getString(_serverIpKey) ?? '';
  bool get _isConnected => _socket != null;

  @override
  void initState() {
    super.initState();
    _connectIfConfigured();
  }

  @override
  void dispose() {
    _messageSubscription?.cancel();
    _socket?.destroy();
    _messageController.dispose();
    super.dispose();
  }

  Future<bool> _connectIfConfigured() async {
    if (_isConnected) return true;
    if (_serverIp.isEmpty) return false;

    setState(() => _isConnecting = true);
    try {
      final socket = await Socket.connect(
        _serverIp,
        _serverPort,
        timeout: const Duration(seconds: 5),
      );
      _socket = socket;
      _messageSubscription = socket
          .cast<List<int>>()
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen(_onMessage, onDone: _onDisconnected, onError: _onSocketError);
      if (mounted) {
        setState(() {});
        _showMessage('Connected to $_serverIp:$_serverPort');
      }
      return true;
    } on SocketException catch (error) {
      if (mounted) _showMessage('Could not connect: ${error.message}');
    } on TimeoutException {
      if (mounted) _showMessage('Connection timed out.');
    } finally {
      if (mounted) setState(() => _isConnecting = false);
    }
    return false;
  }

  void _onMessage(String message) {
    if (!mounted || message.isEmpty) return;
    setState(() => _receivedMessages.insert(0, message));
    _showMessage('Received: $message');
  }

  void _onSocketError(Object _) => _onDisconnected();

  void _onDisconnected() {
    _messageSubscription?.cancel();
    _messageSubscription = null;
    _socket?.destroy();
    _socket = null;
    if (mounted) setState(() {});
  }

  Future<void> _disconnect() async {
    await _messageSubscription?.cancel();
    _messageSubscription = null;
    _socket?.destroy();
    _socket = null;
  }

  Future<void> _sendMessage() async {
    final message = _messageController.text;
    if (message.trim().isEmpty) {
      _showMessage('Enter a message.');
      return;
    }
    if (!await _connectIfConfigured()) {
      if (_serverIp.isEmpty) {
        _showMessage('Set the server IP in Settings first.');
      }
      return;
    }

    try {
      _socket!.write('$message\n');
      await _socket!.flush();
      _messageController.clear();
      _showMessage('Message sent to $_serverIp:$_serverPort');
    } on SocketException {
      _onDisconnected();
      _showMessage('Connection lost.');
    }
  }

  void _showMessage(String text) {
    ScaffoldMessenger.of(context)
      ..clearSnackBars()
      ..showSnackBar(SnackBar(content: Text(text)));
  }

  Future<void> _openSettings() async {
    await Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => SettingsPage(preferences: widget.preferences),
      ),
    );
    await _disconnect();
    if (mounted) {
      setState(() {});
      _connectIfConfigured();
    }
  }

  @override
  Widget build(BuildContext context) {
    final server = _serverIp.isEmpty
        ? 'not configured'
        : '$_serverIp:$_serverPort';
    final connection = _isConnected
        ? 'connected'
        : (_isConnecting ? 'connecting...' : 'offline');
    return Scaffold(
      appBar: AppBar(
        title: const Text('Message Sender'),
        actions: [
          IconButton(
            icon: const Icon(Icons.settings),
            tooltip: 'Settings',
            onPressed: _openSettings,
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('Server: $server ($connection)'),
            const SizedBox(height: 16),
            TextField(
              controller: _messageController,
              minLines: 3,
              maxLines: 5,
              decoration: const InputDecoration(
                border: OutlineInputBorder(),
                hintText: 'Message to send',
              ),
            ),
            const SizedBox(height: 12),
            FilledButton(
              onPressed: _isConnecting ? null : _sendMessage,
              child: const Text('Send'),
            ),
            const SizedBox(height: 20),
            const Text('Received messages'),
            const SizedBox(height: 8),
            Expanded(
              child: ListView.builder(
                itemCount: _receivedMessages.length,
                itemBuilder: (context, index) =>
                    ListTile(title: Text(_receivedMessages[index])),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.preferences});

  final SharedPreferences preferences;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final TextEditingController _ipController;

  @override
  void initState() {
    super.initState();
    _ipController = TextEditingController(
      text: widget.preferences.getString(_serverIpKey) ?? '',
    );
  }

  @override
  void dispose() {
    _ipController.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final ip = _ipController.text.trim();
    if (ip.isEmpty || InternetAddress.tryParse(ip) == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Enter a valid IP address.')),
      );
      return;
    }
    await widget.preferences.setString(_serverIpKey, ip);
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            TextField(
              controller: _ipController,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(
                border: OutlineInputBorder(),
                labelText: 'Server IP address',
                hintText: '192.168.1.10',
              ),
            ),
            const SizedBox(height: 16),
            FilledButton(onPressed: _save, child: const Text('Save')),
          ],
        ),
      ),
    );
  }
}
