# BYOP Computer Vision Project
NAME : ADITYA AMAN

REG NO. : 24BAI10129

SOFTWARE : Live Mobile Object Detection and Face Recognition

Computer Vision (Bring Your Own Project (BYOP) : BL2026270100303 : F11 + F12

---

## Requirements and Usage

- Android phone with camera permission and a Linux/POSIX computer on the same Wi-Fi network.
- Flutter SDK, CMake, a C++17 compiler, OpenCV with DNN support, and `curl`.
- Download the YOLOv5, YuNet, and SFace models before starting the processor.
- Start the server and processor, install the APK, and enter the computer's LAN IP in the app.
- The app shows a simple message screen and live camera view. Start streaming to see detected objects and recognized faces returned to the phone; upload face photos through the server page first when recognition is needed.

# Problem it solves :
Live Mobile Object Detection and Face Recognition is a computer vision project that enables real-time object detection and face recognition using a mobile device. The project addresses the need for efficient and accurate detection of objects and faces in live video streams, which can be useful in various applications such as security, surveillance, and user authentication.

---


# Live Mobile Object Detection and Face Recognition

A Computer Vision project where a phone streams camera frames to a local C++ server. A local OpenCV process performs YOLO object detection, face recognition from a small database, and object tracking. The processed frame returns to the same phone stream.

## Features

- COCO object detection using YOLOv5 Nano
- Face detection and recognition using OpenCV YuNet and SFace
- Face database upload page: first name, UID, and three photos
- Object tracking with stable IDs
- Face decision cached per person track: identity, `Unrecognized`, or retry later
- Session-based routing so processed output returns only to the correct Flutter user
- Flutter chat, saved server IP, camera streaming, and live result display

## Project structure

```text
lib/                    Flutter mobile app
server/                 C++ message, upload, and frame routing server
server_side_process/    C++ OpenCV detection, recognition, and tracking
server/face_database/   Uploaded face reference images
```

## Requirements

- Flutter SDK
- CMake and C++17 compiler
- OpenCV with `dnn`, `imgproc`, `imgcodecs`, and `objdetect`
- Android phone and a Linux/POSIX computer for the server and processor

## Setup

Download models once:

```bash
cd server_side_process
./download_model.sh
./download_face_models.sh
```

Build the app:

```bash
flutter pub get
flutter build apk --release
```

APK path:

```text
build/app/outputs/flutter-apk/app-release.apk
```

## Run

Terminal 1:

```bash
cd server
cmake -S . -B build
cmake --build build
sudo ./build/message_server
```

Terminal 2:

```bash
cd server_side_process
cmake -S . -B build
cmake --build build
./build/yolo_video_detector
```

Install the APK on a phone on the same Wi-Fi. In Settings, enter the server computer's LAN IP, not `localhost` or `127.0.0.1`.

## Face database

Open `http://<server-lan-ip>/`, enter a first name and UID, then upload three clear photos. Files are stored as:

```text
server/face_database/<first-name>.<UID>/1.<extension>
server/face_database/<first-name>.<UID>/2.<extension>
server/face_database/<first-name>.<UID>/3.<extension>
```

Restart `server_side_process` after adding face records.

## Live flow

```text
Flutter -> server TCP 5001 -> local Unix socket -> server_side_process
Flutter <- server TCP 5001 <- local Unix socket <- processed frame
```

Each Flutter stream has a random session ID. The server carries this ID through the vision pipeline and forwards the processed frame only to the matching app session.

## Validation

```bash
flutter analyze
cmake --build server/build
cmake --build server_side_process/build
```
