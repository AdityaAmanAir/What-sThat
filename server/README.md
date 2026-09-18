# Message server

Small Linux/POSIX C++17 TCP server for the Flutter app. It listens on TCP port
`5000` on every network interface, writes received messages to standard output,
and can send messages back to any phone that currently has the app open.

The mobile app sends UTF-8 text terminated by a newline. Each line is printed as:

```text
[client-ip] message
```

## Build and run

```bash
cd server
cmake -S . -B build
cmake --build build
./build/message_server
```

When a phone opens the app, the server prints its IP:

```text
Phone connected: 192.168.1.25
```

Send it a message by typing this into the same server terminal and pressing Enter:

```text
[192.168.1.25] Hello from the server
```

The app displays the received message. If that phone is no longer connected, the
server silently drops the message.

No third-party libraries are required; it uses the POSIX socket APIs provided by
Linux/macOS.

## Face upload page

The same server also serves an upload form on port `80`. Open
`http://<server-ip>/` in a browser, enter a first name and UID, and upload three
images. They are saved as:

```text
face_database/<first-name>.<UID>/1.<extension>
face_database/<first-name>.<UID>/2.<extension>
face_database/<first-name>.<UID>/3.<extension>
```

On Linux, port 80 normally requires elevated permission. Run the server with
`sudo ./build/message_server`, or assign it permission to bind low-numbered ports.

## Live camera stream

The Flutter app streams camera frames to TCP port `5001` after the user taps
**Start stream**. The server forwards each frame, with the phone's IP, through a
local Unix socket to `server_side_process`. Its processed result is routed back
only to that phone's subscribed Flutter connection. No video file is created.
The terminal reports every 10 forwarded frames.

If the phone reports a connection timeout while the server terminal says it is
listening on `0.0.0.0:5001`, allow the port through the Linux firewall:

```bash
sudo ufw allow 5001/tcp
```
