import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:image/image.dart' as image;
import 'package:shared_preferences/shared_preferences.dart';

const _serverIpKey = 'server_ip';
const _serverPort = 5000;
const _videoStreamPort = 5001;
const _udpChunkPayloadSize = 1300;
const _udpHeaderSize = 46;

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
  bool _useTcp = true;
  Socket? _tcpSendSocket;
  Socket? _tcpReceiveSocket;
  StreamSubscription<Uint8List>? _tcpReceiveSubscription;
  final _tcpReceiveBuffer = <int>[];

  RawDatagramSocket? _udpSocket;
  StreamSubscription<RawSocketEvent>? _udpSubscription;
  Uint8List? _serverFrame;
  String _streamId = '';
  String _status = 'Preparing camera...';
  bool _streaming = false;
  bool _encodingFrame = false;
  DateTime _lastFrameAt = DateTime.fromMillisecondsSinceEpoch(0);

  int _outgoingFrameId = 0;
  int _lastCompletedFrameId = 0;
  final Map<int, List<Uint8List?>> _incomingFrames = {};
  final Map<int, int> _incomingFrameTotal = {};
  final Map<int, int> _incomingFrameReceived = {};
  final Map<int, int> _incomingFrameTime = {};
  int _sentFrameCount = 0;
  int _receivedFrameCount = 0;

  String _createStreamId() {
    return List.generate(
      32,
      (_) => Random.secure().nextInt(16).toRadixString(16),
    ).join();
  }

  @override
  void initState() {
    super.initState();
    _streamId = _createStreamId();
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

  void _onTcpFrameData(Uint8List data) {
    _tcpReceiveBuffer.addAll(data);
    while (_tcpReceiveBuffer.length >= 16) {
      if (_tcpReceiveBuffer[0] != 0x46 ||
          _tcpReceiveBuffer[1] != 0x52 ||
          _tcpReceiveBuffer[2] != 0x4d ||
          _tcpReceiveBuffer[3] != 0x31) {
        _tcpReceiveBuffer.clear();
        return;
      }
      final frameLength =
          (_tcpReceiveBuffer[12] << 24) |
          (_tcpReceiveBuffer[13] << 16) |
          (_tcpReceiveBuffer[14] << 8) |
          _tcpReceiveBuffer[15];
      if (frameLength <= 0 || frameLength > 10 * 1024 * 1024) {
        _tcpReceiveBuffer.clear();
        return;
      }
      if (_tcpReceiveBuffer.length < 16 + frameLength) return;
      final frame = Uint8List.fromList(
        _tcpReceiveBuffer.sublist(16, 16 + frameLength),
      );
      _tcpReceiveBuffer.removeRange(0, 16 + frameLength);
      _receivedFrameCount++;
      if (mounted) {
        setState(() {
          _serverFrame = frame;
          _status = 'Streaming TCP (sent: $_sentFrameCount, recv: $_receivedFrameCount)';
        });
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

    _streamId = _createStreamId();

    if (_useTcp) {
      if (mounted) {
        setState(() => _status = 'Connecting TCP stream...');
      }
      try {
        final recvSocket = await Socket.connect(
          widget.serverIp,
          _videoStreamPort,
          timeout: const Duration(seconds: 4),
        );
        recvSocket.setOption(SocketOption.tcpNoDelay, true);
        _tcpReceiveSocket = recvSocket;
        recvSocket.add(utf8.encode('SUB1$_streamId'));
        await recvSocket.flush();

        _tcpReceiveBuffer.clear();
        _tcpReceiveSubscription = recvSocket.listen(
          _onTcpFrameData,
          onDone: () {
            if (mounted && _streaming) {
              setState(() => _status = 'TCP stream ended by server');
            }
          },
          onError: (e) {
            if (mounted && _streaming) {
              setState(() => _status = 'TCP receive error: $e');
            }
          },
        );

        final sendSocket = await Socket.connect(
          widget.serverIp,
          _videoStreamPort,
          timeout: const Duration(seconds: 4),
        );
        sendSocket.setOption(SocketOption.tcpNoDelay, true);
        _tcpSendSocket = sendSocket;
        sendSocket.add(utf8.encode('STR1$_streamId'));
        await sendSocket.flush();

        _sentFrameCount = 0;
        _receivedFrameCount = 0;
        await _camera!.startImageStream(_onCameraImage);
        if (mounted) {
          setState(() {
            _streaming = true;
            _status = 'Streaming TCP (sent: 0, recv: 0)';
          });
        }
      } catch (e) {
        if (mounted) {
          setState(() => _status = 'TCP stream failed: $e');
        }
        await _stopStream();
      }
    } else {
      if (mounted) {
        setState(() => _status = 'Checking server connection via TCP...');
      }

      // 1. TCP connection check first
      try {
        final tcpSocket = await Socket.connect(
          widget.serverIp,
          _videoStreamPort,
          timeout: const Duration(seconds: 4),
        );
        tcpSocket.add(utf8.encode('CHK1$_streamId'));
        await tcpSocket.flush();

        final responseCompleter = Completer<void>();
        final sub = tcpSocket.listen(
          (_) {
            if (!responseCompleter.isCompleted) responseCompleter.complete();
          },
          onDone: () {
            if (!responseCompleter.isCompleted) responseCompleter.complete();
          },
          onError: (err) {
            if (!responseCompleter.isCompleted) responseCompleter.completeError(err);
          },
        );

        await responseCompleter.future.timeout(
          const Duration(seconds: 4),
          onTimeout: () => null,
        );
        await sub.cancel();
        await tcpSocket.close();
        tcpSocket.destroy();
      } on SocketException catch (error) {
        if (mounted) {
          setState(() => _status = 'Could not connect via TCP: ${error.message}');
        }
        return;
      } on TimeoutException {
        if (mounted) {
          setState(() => _status = 'TCP connection check timed out.');
        }
        return;
      } catch (error) {
        if (mounted) {
          setState(() => _status = 'Connection check failed: $error');
        }
        return;
      }

      // 2. Low-latency UDP socket
      try {
        final udpSocket = await RawDatagramSocket.bind(
          InternetAddress.anyIPv4,
          0,
        );
        _udpSocket = udpSocket;
        _incomingFrames.clear();
        _incomingFrameTotal.clear();
        _incomingFrameReceived.clear();
        _incomingFrameTime.clear();
        _lastCompletedFrameId = 0;
        _sentFrameCount = 0;
        _receivedFrameCount = 0;

        _udpSubscription = udpSocket.listen((event) {
          if (event == RawSocketEvent.read) {
            Datagram? datagram;
            while ((datagram = udpSocket.receive()) != null) {
              _onUdpPacket(datagram!.data);
            }
          }
        });

        // Send initial UDP packet so the server registers client endpoint immediately
        final initPacket = Uint8List(_udpHeaderSize);
        initPacket[0] = 0x55; // 'U'
        initPacket[1] = 0x44; // 'D'
        initPacket[2] = 0x50; // 'P'
        initPacket[3] = 0x31; // '1'
        initPacket.setRange(4, 36, ascii.encode(_streamId));
        udpSocket.send(initPacket, InternetAddress(widget.serverIp), _videoStreamPort);

        await _camera!.startImageStream(_onCameraImage);
        if (mounted) {
          setState(() {
            _streaming = true;
            _status = 'Streaming UDP (sent: 0, recv: 0)';
          });
        }
      } catch (error) {
        if (mounted) {
          setState(() => _status = 'UDP stream start failed: $error');
        }
        await _stopStream();
      }
    }
  }

  void _onUdpPacket(Uint8List packet) {
    if (packet.length < _udpHeaderSize) return;
    if (packet[0] != 0x55 ||
        packet[1] != 0x44 ||
        packet[2] != 0x50 ||
        packet[3] != 0x31) {
      return;
    }

    final byteData = ByteData.sublistView(packet);
    final frameId = byteData.getUint32(36, Endian.big);
    final chunkIndex = byteData.getUint16(40, Endian.big);
    final totalChunks = byteData.getUint16(42, Endian.big);
    final payloadLen = byteData.getUint16(44, Endian.big);

    if (totalChunks == 0 || chunkIndex >= totalChunks || payloadLen == 0) return;
    if (packet.length < _udpHeaderSize + payloadLen) return;

    if (frameId <= _lastCompletedFrameId) {
      if (_lastCompletedFrameId - frameId > 5) {
        // Stream restarted or counter reset
        _lastCompletedFrameId = 0;
      } else {
        return;
      }
    }

    final now = DateTime.now().millisecondsSinceEpoch;

    // Clean up expired in-flight frames older than 1.5 seconds
    _incomingFrameTime.removeWhere((id, t) {
      if (now - t > 1500) {
        _incomingFrames.remove(id);
        _incomingFrameTotal.remove(id);
        _incomingFrameReceived.remove(id);
        return true;
      }
      return false;
    });

    if (!_incomingFrames.containsKey(frameId)) {
      if (_incomingFrames.length > 5) {
        final oldestId = _incomingFrames.keys.reduce((a, b) => a < b ? a : b);
        _incomingFrames.remove(oldestId);
        _incomingFrameTotal.remove(oldestId);
        _incomingFrameReceived.remove(oldestId);
        _incomingFrameTime.remove(oldestId);
      }
      _incomingFrames[frameId] = List<Uint8List?>.filled(totalChunks, null);
      _incomingFrameTotal[frameId] = totalChunks;
      _incomingFrameReceived[frameId] = 0;
      _incomingFrameTime[frameId] = now;
    }

    final chunks = _incomingFrames[frameId]!;
    if (chunkIndex < chunks.length && chunks[chunkIndex] == null) {
      chunks[chunkIndex] = packet.sublist(
        _udpHeaderSize,
        _udpHeaderSize + payloadLen,
      );
      final count = (_incomingFrameReceived[frameId] ?? 0) + 1;
      _incomingFrameReceived[frameId] = count;

      if (count == totalChunks) {
        final totalBytes = chunks.fold<int>(
          0,
          (sum, c) => sum + (c?.length ?? 0),
        );
        final fullFrame = Uint8List(totalBytes);
        var offset = 0;
        for (final c in chunks) {
          if (c != null) {
            fullFrame.setRange(offset, offset + c.length, c);
            offset += c.length;
          }
        }

        _lastCompletedFrameId = frameId;
        _incomingFrames.removeWhere((id, _) => id <= frameId);
        _incomingFrameTotal.removeWhere((id, _) => id <= frameId);
        _incomingFrameReceived.removeWhere((id, _) => id <= frameId);
        _incomingFrameTime.removeWhere((id, _) => id <= frameId);

        if (fullFrame.length >= 4 &&
            fullFrame[0] == 0xFF &&
            fullFrame[1] == 0xD8) {
          _receivedFrameCount++;
          if (mounted) {
            setState(() {
              _serverFrame = fullFrame;
              _status =
                  'Streaming UDP (sent: $_sentFrameCount, recv: $_receivedFrameCount)';
            });
          }
        }
      }
    }
  }

  void _onCameraImage(CameraImage cameraImage) {
    if (_encodingFrame ||
        DateTime.now().difference(_lastFrameAt) <
            const Duration(milliseconds: 100)) {
      return;
    }
    _encodingFrame = true;
    _lastFrameAt = DateTime.now();
    _sendFrame(cameraImage).whenComplete(() => _encodingFrame = false);
  }

  int _getImageRotation() {
    final camera = _camera;
    if (camera == null || !camera.value.isInitialized) return 0;

    final sensorOrientation = camera.description.sensorOrientation;
    final isFrontFacing =
        camera.description.lensDirection == CameraLensDirection.front;
    final deviceOrientation = camera.value.lockedCaptureOrientation ??
        camera.value.deviceOrientation;

    int angle = 0;
    switch (deviceOrientation) {
      case DeviceOrientation.portraitUp:
        angle = 0;
        break;
      case DeviceOrientation.portraitDown:
        angle = 180;
        break;
      case DeviceOrientation.landscapeLeft:
        angle = 270;
        break;
      case DeviceOrientation.landscapeRight:
        angle = 90;
        break;
    }

    if (isFrontFacing) {
      angle = -angle;
    }

    return (angle + sensorOrientation + 360) % 360;
  }

  Future<void> _sendFrame(CameraImage cameraImage) async {
    if (!_streaming) return;

    try {
      final (jpeg, width, height) = _cameraImageToJpeg(cameraImage);

      if (_useTcp) {
        final sendSocket = _tcpSendSocket;
        if (sendSocket == null) return;
        final header = ByteData(16)
          ..setUint8(0, 0x46) // 'F'
          ..setUint8(1, 0x52) // 'R'
          ..setUint8(2, 0x4d) // 'M'
          ..setUint8(3, 0x31) // '1'
          ..setUint32(4, width, Endian.big)
          ..setUint32(8, height, Endian.big)
          ..setUint32(12, jpeg.length, Endian.big);
        sendSocket.add(header.buffer.asUint8List());
        sendSocket.add(jpeg);
        await sendSocket.flush();
        _sentFrameCount++;
        if (mounted && (_sentFrameCount == 1 || _sentFrameCount % 10 == 0)) {
          setState(() {
            _status =
                'Streaming TCP (sent: $_sentFrameCount, recv: $_receivedFrameCount)';
          });
        }
      } else {
        final udpSocket = _udpSocket;
        if (udpSocket == null) return;
        final totalChunks =
            (jpeg.length + _udpChunkPayloadSize - 1) ~/ _udpChunkPayloadSize;
        if (totalChunks == 0 || totalChunks > 65535) return;

        final frameId = ++_outgoingFrameId;
        final sessionBytes = ascii.encode(_streamId);
        final targetAddress = InternetAddress(widget.serverIp);

        for (var chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex) {
          final offset = chunkIndex * _udpChunkPayloadSize;
          final len = (offset + _udpChunkPayloadSize <= jpeg.length)
              ? _udpChunkPayloadSize
              : (jpeg.length - offset);

          final packet = Uint8List(_udpHeaderSize + len);
          packet[0] = 0x55; // 'U'
          packet[1] = 0x44; // 'D'
          packet[2] = 0x50; // 'P'
          packet[3] = 0x31; // '1'
          packet.setRange(4, 36, sessionBytes);

          final byteData = ByteData.sublistView(packet);
          byteData.setUint32(36, frameId, Endian.big);
          byteData.setUint16(40, chunkIndex, Endian.big);
          byteData.setUint16(42, totalChunks, Endian.big);
          byteData.setUint16(44, len, Endian.big);
          packet.setRange(_udpHeaderSize, _udpHeaderSize + len,
              jpeg.sublist(offset, offset + len));

          udpSocket.send(packet, targetAddress, _videoStreamPort);
          if (chunkIndex % 4 == 3) {
            await Future<void>.delayed(Duration.zero);
          }
        }
        _sentFrameCount++;
        if (mounted && (_sentFrameCount == 1 || _sentFrameCount % 10 == 0)) {
          setState(() {
            _status =
                'Streaming UDP (sent: $_sentFrameCount, recv: $_receivedFrameCount)';
            if (_sentFrameCount >= 10 && _receivedFrameCount == 0) {
              _status += ' (UDP blocked? Switch to TCP)';
            }
          });
        }
      }
    } catch (e) {
      if (mounted) {
        setState(() => _status = 'Frame send error: $e');
      }
    }
  }

  (Uint8List, int, int) _cameraImageToJpeg(CameraImage cameraImage) {
    if (cameraImage.format.group == ImageFormatGroup.jpeg) {
      return (cameraImage.planes[0].bytes, cameraImage.width, cameraImage.height);
    }

    final srcWidth = cameraImage.width;
    final srcHeight = cameraImage.height;

    // Downsample if image is large to guarantee ultra-fast conversion (under 15ms)
    final step = (srcWidth > 400 || srcHeight > 400) ? 2 : 1;
    final dstWidth = (srcWidth + step - 1) ~/ step;
    final dstHeight = (srcHeight + step - 1) ~/ step;

    if (cameraImage.format.group == ImageFormatGroup.bgra8888) {
      final img = image.Image.fromBytes(
        width: srcWidth,
        height: srcHeight,
        bytes: cameraImage.planes[0].bytes.buffer,
        order: image.ChannelOrder.bgra,
      );
      final scaled = (step > 1)
          ? image.copyResize(img, width: dstWidth, height: dstHeight)
          : img;
      final rotation = _getImageRotation();
      final rotated =
          rotation != 0 ? image.copyRotate(scaled, angle: rotation) : scaled;
      return (
        Uint8List.fromList(image.encodeJpg(rotated, quality: 60)),
        rotated.width,
        rotated.height
      );
    }

    if (cameraImage.planes.isEmpty) {
      throw const FormatException('Camera image has no planes');
    }

    var output = image.Image(width: dstWidth, height: dstHeight);
    final yPlane = cameraImage.planes[0];
    final yBytes = yPlane.bytes;
    final yRowStride = yPlane.bytesPerRow;

    if (cameraImage.planes.length >= 3) {
      final uPlane = cameraImage.planes[1];
      final vPlane = cameraImage.planes[2];
      final uBytes = uPlane.bytes;
      final vBytes = vPlane.bytes;
      final uRowStride = uPlane.bytesPerRow;
      final vRowStride = vPlane.bytesPerRow;
      final uPixelStride = uPlane.bytesPerPixel ?? 1;
      final vPixelStride = vPlane.bytesPerPixel ?? 1;

      for (var outY = 0; outY < dstHeight; outY++) {
        final srcY = outY * step;
        final yRowOffset = srcY * yRowStride;
        final uvRow = srcY ~/ 2;
        final uRowOffset = uvRow * uRowStride;
        final vRowOffset = uvRow * vRowStride;

        for (var outX = 0; outX < dstWidth; outX++) {
          final srcX = outX * step;
          final yIndex = yRowOffset + srcX;
          if (yIndex >= yBytes.length) break;
          final yValue = yBytes[yIndex];

          final uvColumn = srcX ~/ 2;
          final uIndex = uRowOffset + uvColumn * uPixelStride;
          final vIndex = vRowOffset + uvColumn * vPixelStride;

          final uValue = (uIndex < uBytes.length) ? uBytes[uIndex] : 128;
          final vValue = (vIndex < vBytes.length) ? vBytes[vIndex] : 128;

          final red = (yValue + 1.402 * (vValue - 128)).round().clamp(0, 255);
          final green =
              (yValue - 0.344136 * (uValue - 128) - 0.714136 * (vValue - 128))
                  .round()
                  .clamp(0, 255);
          final blue = (yValue + 1.772 * (uValue - 128)).round().clamp(0, 255);
          output.setPixelRgb(outX, outY, red, green, blue);
        }
      }
    } else if (cameraImage.planes.length == 2) {
      final uvPlane = cameraImage.planes[1];
      final uvBytes = uvPlane.bytes;
      final uvRowStride = uvPlane.bytesPerRow;
      final uvPixelStride = uvPlane.bytesPerPixel ?? 2;

      for (var outY = 0; outY < dstHeight; outY++) {
        final srcY = outY * step;
        final yRowOffset = srcY * yRowStride;
        final uvRow = srcY ~/ 2;
        final uvRowOffset = uvRow * uvRowStride;

        for (var outX = 0; outX < dstWidth; outX++) {
          final srcX = outX * step;
          final yIndex = yRowOffset + srcX;
          if (yIndex >= yBytes.length) break;
          final yValue = yBytes[yIndex];

          final uvColumn = srcX ~/ 2;
          final uvIndex = uvRowOffset + uvColumn * uvPixelStride;

          final vValue = (uvIndex < uvBytes.length) ? uvBytes[uvIndex] : 128;
          final uValue = (uvIndex + 1 < uvBytes.length) ? uvBytes[uvIndex + 1] : 128;

          final red = (yValue + 1.402 * (vValue - 128)).round().clamp(0, 255);
          final green =
              (yValue - 0.344136 * (uValue - 128) - 0.714136 * (vValue - 128))
                  .round()
                  .clamp(0, 255);
          final blue = (yValue + 1.772 * (uValue - 128)).round().clamp(0, 255);
          output.setPixelRgb(outX, outY, red, green, blue);
        }
      }
    } else {
      for (var outY = 0; outY < dstHeight; outY++) {
        final srcY = outY * step;
        final yRowOffset = srcY * yRowStride;
        for (var outX = 0; outX < dstWidth; outX++) {
          final srcX = outX * step;
          final yIndex = yRowOffset + srcX;
          final val = (yIndex < yBytes.length) ? yBytes[yIndex] : 0;
          output.setPixelRgb(outX, outY, val, val, val);
        }
      }
    }

    final rotation = _getImageRotation();
    if (rotation != 0) {
      output = image.copyRotate(output, angle: rotation);
    }
    final bytes = Uint8List.fromList(image.encodeJpg(output, quality: 60));
    return (bytes, output.width, output.height);
  }

  Future<void> _stopStream() async {
    final camera = _camera;
    if (camera != null && camera.value.isStreamingImages) {
      await camera.stopImageStream();
    }
    await _udpSubscription?.cancel();
    _udpSubscription = null;
    _udpSocket?.close();
    _udpSocket = null;

    await _tcpReceiveSubscription?.cancel();
    _tcpReceiveSubscription = null;
    try {
      await _tcpReceiveSocket?.close();
    } catch (_) {}
    _tcpReceiveSocket?.destroy();
    _tcpReceiveSocket = null;

    try {
      await _tcpSendSocket?.close();
    } catch (_) {}
    _tcpSendSocket?.destroy();
    _tcpSendSocket = null;

    _tcpReceiveBuffer.clear();
    _incomingFrames.clear();
    _incomingFrameTotal.clear();
    _incomingFrameReceived.clear();
    _incomingFrameTime.clear();
    if (mounted) {
      setState(() {
        _streaming = false;
        _status = 'Ready';
      });
    }
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
    final isLandscape =
        MediaQuery.of(context).orientation == Orientation.landscape;

    final previewWidget = (camera != null && camera.value.isInitialized)
        ? Center(
            child: AspectRatio(
              aspectRatio: isLandscape
                  ? camera.value.aspectRatio
                  : (1 / camera.value.aspectRatio),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: CameraPreview(camera),
              ),
            ),
          )
        : const SizedBox(
            height: 200,
            child: Center(child: CircularProgressIndicator()),
          );

    final incomingFrameWidget = _serverFrame != null
        ? Image.memory(
            _serverFrame!,
            fit: BoxFit.contain,
            gaplessPlayback: true,
            errorBuilder: (context, error, stackTrace) => Center(
              child: Text('Frame decode error: $error'),
            ),
          )
        : (_streaming
            ? const Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  SizedBox(
                    width: 24,
                    height: 24,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                  SizedBox(height: 8),
                  Text('Waiting for incoming server frames...'),
                ],
              )
            : const SizedBox(height: 40, child: Center(child: Text('Idle'))));

    final protocolSelector = SegmentedButton<bool>(
      segments: const [
        ButtonSegment(
          value: true,
          label: Text('TCP (Reliable)'),
          icon: Icon(Icons.shield),
        ),
        ButtonSegment(
          value: false,
          label: Text('UDP (Fast)'),
          icon: Icon(Icons.bolt),
        ),
      ],
      selected: {_useTcp},
      onSelectionChanged: _streaming
          ? null
          : (val) => setState(() => _useTcp = val.first),
    );

    Widget body;
    if (isLandscape) {
      body = Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Expanded(
            flex: 5,
            child: Center(child: previewWidget),
          ),
          const SizedBox(width: 16),
          Expanded(
            flex: 5,
            child: SingleChildScrollView(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(_status),
                  const SizedBox(height: 12),
                  protocolSelector,
                  const SizedBox(height: 12),
                  FilledButton(
                    onPressed: camera == null ? null : _toggleStream,
                    child: Text(_streaming ? 'Stop stream' : 'Start stream'),
                  ),
                  const SizedBox(height: 12),
                  const Text('Incoming server frames'),
                  const SizedBox(height: 8),
                  SizedBox(
                    height: 140,
                    child: Center(child: incomingFrameWidget),
                  ),
                ],
              ),
            ),
          ),
        ],
      );
    } else {
      body = SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(_status),
            const SizedBox(height: 12),
            previewWidget,
            const SizedBox(height: 16),
            protocolSelector,
            const SizedBox(height: 12),
            FilledButton(
              onPressed: camera == null ? null : _toggleStream,
              child: Text(_streaming ? 'Stop stream' : 'Start stream'),
            ),
            const SizedBox(height: 16),
            const Text('Incoming server frames'),
            const SizedBox(height: 8),
            SizedBox(
              height: 160,
              child: Center(child: incomingFrameWidget),
            ),
          ],
        ),
      );
    }

    return Scaffold(
      appBar: AppBar(title: const Text('Live camera stream')),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: body,
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
    final isLandscape =
        MediaQuery.of(context).orientation == Orientation.landscape;

    Widget body;
    if (isLandscape) {
      body = Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Expanded(
            flex: 5,
            child: SingleChildScrollView(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text('Server: $server ($connection)'),
                  const SizedBox(height: 12),
                  TextField(
                    controller: _messageController,
                    minLines: 2,
                    maxLines: 4,
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
                ],
              ),
            ),
          ),
          const SizedBox(width: 16),
          Expanded(
            flex: 5,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
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
        ],
      );
    } else {
      body = Column(
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
      );
    }

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
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: body,
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
      body: SafeArea(
        child: SingleChildScrollView(
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
      ),
    );
  }
}
