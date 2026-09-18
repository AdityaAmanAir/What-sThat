import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:image/image.dart' as image;
import 'package:shared_preferences/shared_preferences.dart';

const _serverIpKey = 'server_ip';
const _serverPort = 5000;
const _videoStreamPort = 5001;

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

class VideoStreamPage extends StatefulWidget {
  const VideoStreamPage({super.key, required this.serverIp});

  final String serverIp;

  @override
  State<VideoStreamPage> createState() => _VideoStreamPageState();
}

class _VideoStreamPageState extends State<VideoStreamPage> {
  CameraController? _camera;
  Socket? _socket;
  Socket? _receiveSocket;
  StreamSubscription<Uint8List>? _receiveSubscription;
  final _receiveBuffer = <int>[];
  Uint8List? _serverFrame;
  late final String _streamId;
  String _status = 'Preparing camera...';
  bool _streaming = false;
  bool _encodingFrame = false;
  DateTime _lastFrameAt = DateTime.fromMillisecondsSinceEpoch(0);

  @override
  void initState() {
    super.initState();
    _streamId = List.generate(
      32,
      (_) => Random.secure().nextInt(16).toRadixString(16),
    ).join();
    _prepareCamera();
  }

  Future<void> _prepareCamera() async {
    try {
      final cameras = await availableCameras();
      if (cameras.isEmpty) {
        throw CameraException('no-camera', 'No camera found.');
      }
      final camera = CameraController(
        cameras.first,
        ResolutionPreset.low,
        enableAudio: false,
        imageFormatGroup: ImageFormatGroup.yuv420,
      );
      await camera.initialize();
      if (!mounted) {
        await camera.dispose();
        return;
      }
      setState(() {
        _camera = camera;
        _status = 'Ready';
      });
    } on CameraException catch (error) {
      if (mounted) {
        setState(() => _status = 'Camera error: ${error.description}');
      }
    }
  }

  Future<void> _toggleStream() async {
    if (_streaming) {
      await _stopStream();
      return;
    }
    if (_camera == null || !_camera!.value.isInitialized) {
      return;
    }
    try {
      _receiveSocket = await Socket.connect(
        widget.serverIp,
        _videoStreamPort,
        timeout: const Duration(seconds: 5),
      );
      _receiveSocket!.add(utf8.encode('SUB1$_streamId'));
      await _receiveSocket!.flush();
      _receiveSubscription = _receiveSocket!.listen(
        _onServerFrameData,
        onDone: _onServerFeedClosed,
        onError: (_, _) => _onServerFeedClosed(),
      );
      _socket = await Socket.connect(
        widget.serverIp,
        _videoStreamPort,
        timeout: const Duration(seconds: 5),
      );
      _socket!.add(utf8.encode('STR1$_streamId'));
      await _socket!.flush();
      await _camera!.startImageStream(_onCameraImage);
      if (mounted) {
        setState(() {
          _streaming = true;
          _status = 'Streaming to ${widget.serverIp}:$_videoStreamPort';
        });
      }
    } on SocketException catch (error) {
      if (mounted) {
        setState(() => _status = 'Could not connect: ${error.message}');
      }
      await _stopStream();
    } on CameraException catch (error) {
      if (mounted) {
        setState(() => _status = 'Camera error: ${error.description}');
      }
      await _stopStream();
    }
  }

  void _onServerFrameData(Uint8List data) {
    _receiveBuffer.addAll(data);
    while (_receiveBuffer.length >= 16) {
      if (_receiveBuffer[0] != 0x46 ||
          _receiveBuffer[1] != 0x52 ||
          _receiveBuffer[2] != 0x4d ||
          _receiveBuffer[3] != 0x31) {
        _receiveBuffer.clear();
        return;
      }
      final frameLength =
          (_receiveBuffer[12] << 24) |
          (_receiveBuffer[13] << 16) |
          (_receiveBuffer[14] << 8) |
          _receiveBuffer[15];
      if (frameLength <= 0 || frameLength > 10 * 1024 * 1024) {
        _receiveBuffer.clear();
        return;
      }
      if (_receiveBuffer.length < 16 + frameLength) return;
      final frame = Uint8List.fromList(
        _receiveBuffer.sublist(16, 16 + frameLength),
      );
      _receiveBuffer.removeRange(0, 16 + frameLength);
      if (mounted) setState(() => _serverFrame = frame);
    }
  }

  void _onServerFeedClosed() {
    if (mounted && _streaming) {
      setState(() => _status = 'Streaming; no incoming server frames');
    }
  }

  void _onCameraImage(CameraImage cameraImage) {
    if (_encodingFrame ||
        DateTime.now().difference(_lastFrameAt) <
            const Duration(milliseconds: 200)) {
      return;
    }
    _encodingFrame = true;
    _lastFrameAt = DateTime.now();
    _sendFrame(cameraImage).whenComplete(() => _encodingFrame = false);
  }

  Future<void> _sendFrame(CameraImage cameraImage) async {
    final socket = _socket;
    if (socket == null) return;
    try {
      final jpeg = _cameraImageToJpeg(cameraImage);
      final header = ByteData(16)
        ..setUint8(0, 0x46)
        ..setUint8(1, 0x52)
        ..setUint8(2, 0x4d)
        ..setUint8(3, 0x31)
        ..setUint32(4, cameraImage.width, Endian.big)
        ..setUint32(8, cameraImage.height, Endian.big)
        ..setUint32(12, jpeg.length, Endian.big);
      socket.add(header.buffer.asUint8List());
      socket.add(jpeg);
      await socket.flush();
    } on SocketException {
      if (mounted) {
        setState(() => _status = 'Stream connection lost');
        _streaming = false;
      }
      await _stopStream();
    }
  }

  Uint8List _cameraImageToJpeg(CameraImage cameraImage) {
    if (cameraImage.format.group != ImageFormatGroup.yuv420) {
      throw const FormatException(
        'This prototype supports Android YUV420 camera frames.',
      );
    }
    final output = image.Image(
      width: cameraImage.width,
      height: cameraImage.height,
    );
    final yPlane = cameraImage.planes[0];
    final uPlane = cameraImage.planes[1];
    final vPlane = cameraImage.planes[2];
    for (var y = 0; y < cameraImage.height; y++) {
      for (var x = 0; x < cameraImage.width; x++) {
        final yValue = yPlane.bytes[y * yPlane.bytesPerRow + x];
        final uvRow = y ~/ 2;
        final uvColumn = x ~/ 2;
        final uValue =
            uPlane.bytes[uvRow * uPlane.bytesPerRow +
                uvColumn * uPlane.bytesPerPixel!];
        final vValue =
            vPlane.bytes[uvRow * vPlane.bytesPerRow +
                uvColumn * vPlane.bytesPerPixel!];
        final red = (yValue + 1.402 * (vValue - 128)).round().clamp(0, 255);
        final green =
            (yValue - 0.344136 * (uValue - 128) - 0.714136 * (vValue - 128))
                .round()
                .clamp(0, 255);
        final blue = (yValue + 1.772 * (uValue - 128)).round().clamp(0, 255);
        output.setPixelRgb(x, y, red, green, blue);
      }
    }
    return Uint8List.fromList(image.encodeJpg(output, quality: 70));
  }

  Future<void> _stopStream() async {
    final camera = _camera;
    if (camera != null && camera.value.isStreamingImages) {
      await camera.stopImageStream();
    }
    await _socket?.close();
    _socket?.destroy();
    _socket = null;
    await _receiveSubscription?.cancel();
    _receiveSubscription = null;
    await _receiveSocket?.close();
    _receiveSocket?.destroy();
    _receiveSocket = null;
    _receiveBuffer.clear();
    if (mounted) setState(() => _streaming = false);
  }

  @override
  void dispose() {
    _stopStream();
    _camera?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final camera = _camera;
    return Scaffold(
      appBar: AppBar(title: const Text('Live camera stream')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(_status),
            const SizedBox(height: 16),
            if (camera != null && camera.value.isInitialized)
              AspectRatio(
                aspectRatio: camera.value.aspectRatio,
                child: CameraPreview(camera),
              )
            else
              const Expanded(child: Center(child: CircularProgressIndicator())),
            const SizedBox(height: 16),
            FilledButton(
              onPressed: camera == null ? null : _toggleStream,
              child: Text(_streaming ? 'Stop stream' : 'Start stream'),
            ),
            const SizedBox(height: 16),
            const Text('Incoming server frames'),
            const SizedBox(height: 8),
            if (_serverFrame != null)
              SizedBox(
                height: 160,
                child: Image.memory(_serverFrame!, fit: BoxFit.contain),
              )
            else
              const SizedBox(height: 40, child: Center(child: Text('Idle'))),
          ],
        ),
      ),
    );
  }
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

  Future<void> _openVideoStream() async {
    if (_serverIp.isEmpty) {
      _showMessage('Set the server IP in Settings first.');
      return;
    }
    await Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => VideoStreamPage(serverIp: _serverIp),
      ),
    );
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
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: _openVideoStream,
              icon: const Icon(Icons.videocam),
              label: const Text('Stream camera to server'),
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
