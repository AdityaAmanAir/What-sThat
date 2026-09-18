# YOLO COCO video detector

This is a minimal C++17/OpenCV DNN process using COCO-trained YOLOv5 Nano.

1. Place one video in `source/` named `test` with any video extension, for example
   `source/test.mp4`.
2. Download the model once:

   ```bash
   ./download_model.sh
   ./download_face_models.sh
   ```

3. Build and run:

   ```bash
   cmake -S . -B build
   cmake --build build
   ./build/yolo_video_detector
   ```

The annotated video is written to `output/test_detected.mp4`.

Face recognition reads the three uploaded images per person from
`../server/face_database/<first-name>.<UID>/`. When a tracked person has a clear
face match, their folder name is shown once and cached for that track. Clear
non-matches are shown as red `Unrecognized`; uncertain faces are tried again on a
later frame.

Requirements: CMake, a C++17 compiler, OpenCV built with the DNN module, FFmpeg,
and `curl` for the one-time model download. FFmpeg handles video reading and
writing, so normal phone H.264 MP4/MOV videos work even when OpenCV lacks its
FFmpeg video plugin. The output is H.264 MP4 without audio.
