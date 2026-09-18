# YOLO COCO video detector

This is a minimal C++17/OpenCV DNN process using COCO-trained YOLOv5 Nano.

1. Start the main server first. It receives Flutter camera frames on port `5001`
   and passes them locally through `/tmp/whats_that_processor.sock`.
2. Download the models once:

   ```bash
   ./download_model.sh
   ./download_face_models.sh
   ```

3. Build and run the live processor on the same machine as `server`:

   ```bash
   cmake -S . -B build
   cmake --build build
   ./build/yolo_video_detector
   ```

Each received live camera frame is detected, face-recognized, and tracked. The
processed JPEG is returned to `server`, with the originating phone IP preserved;
the server forwards it only to that phone's Flutter app. No source video or
output image file is used.

Face recognition reads the three uploaded images per person from
`../server/face_database/<first-name>.<UID>/`. When a tracked person has a clear
face match, their folder name is shown once and cached for that track. Clear
non-matches are shown as red `Unrecognized`; uncertain faces are tried again on a
later frame.

Requirements: CMake, a C++17 compiler, OpenCV built with the DNN module, and
`curl` for the one-time model download.

The processor configures OpenCV to use all CPU cores available to the machine
for each ordered frame's DNN/image work. Frames stay ordered per Flutter session
so object tracking remains correct.
