#!/usr/bin/env bash
set -euo pipefail

mkdir -p models
curl -L --fail --progress-bar \
  https://github.com/ultralytics/yolov5/releases/download/v7.0/yolov5n.onnx \
  -o models/yolov5n.onnx
