#!/usr/bin/env bash
set -euo pipefail

mkdir -p models
curl -L --fail --progress-bar \
  https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx \
  -o models/face_detection_yunet_2023mar.onnx
curl -L --fail --progress-bar \
  https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx \
  -o models/face_recognition_sface_2021dec.onnx
