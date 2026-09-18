#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

namespace fs = std::filesystem;

namespace {

constexpr int kInputSize = 640;
constexpr float kConfidenceThreshold = 0.40F;
constexpr float kNmsThreshold = 0.45F;
constexpr double kRecognizedFaceThreshold = 0.363;
constexpr double kUnrecognizedFaceThreshold = 0.25;

const std::vector<std::string> kCocoClasses = {
    "person",       "bicycle",      "car",           "motorcycle", "airplane",
    "bus",          "train",        "truck",         "boat",       "traffic light",
    "fire hydrant", "stop sign",    "parking meter", "bench",      "bird",
    "cat",          "dog",          "horse",         "sheep",      "cow",
    "elephant",     "bear",         "zebra",         "giraffe",    "backpack",
    "umbrella",     "handbag",      "tie",           "suitcase",   "frisbee",
    "skis",         "snowboard",    "sports ball",   "kite",       "baseball bat",
    "baseball glove", "skateboard", "surfboard",      "tennis racket", "bottle",
    "wine glass",   "cup",          "fork",          "knife",      "spoon",
    "bowl",         "banana",       "apple",         "sandwich",   "orange",
    "broccoli",     "carrot",       "hot dog",       "pizza",      "donut",
    "cake",         "chair",        "couch",         "potted plant", "bed",
    "dining table", "toilet",       "tv",            "laptop",    "mouse",
    "remote",       "keyboard",     "cell phone",    "microwave",  "oven",
    "toaster",      "sink",         "refrigerator",  "book",       "clock",
    "vase",         "scissors",     "teddy bear",    "hair drier", "toothbrush",
};

fs::path findInputVideo() {
  const fs::path sourceDirectory = "source";
  if (!fs::is_directory(sourceDirectory)) return {};

  for (const auto& entry : fs::directory_iterator(sourceDirectory)) {
    if (entry.is_regular_file() && entry.path().stem() == "test") {
      return entry.path();
    }
  }
  return {};
}

std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (char character : value) {
    quoted += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return quoted + "'";
}

struct VideoInfo {
  int width;
  int height;
  double fps;
};

VideoInfo probeVideo(const fs::path& path) {
  const std::string command =
      "ffprobe -v error -select_streams v:0 -show_entries "
      "stream=width,height,r_frame_rate -of default=noprint_wrappers=1:nokey=1 " +
      shellQuote(path.string());
  FILE* probe = popen(command.c_str(), "r");
  if (probe == nullptr) throw std::runtime_error("Unable to run ffprobe.");

  char line[128];
  std::vector<std::string> values;
  while (fgets(line, sizeof(line), probe) != nullptr) {
    std::string value(line);
    if (!value.empty() && value.back() == '\n') value.pop_back();
    values.push_back(value);
  }
  if (pclose(probe) != 0 || values.size() != 3) {
    throw std::runtime_error("Unable to read video properties with ffprobe.");
  }

  const std::size_t slash = values[2].find('/');
  const double fps = slash == std::string::npos
                         ? std::stod(values[2])
                         : std::stod(values[2].substr(0, slash)) /
                               std::stod(values[2].substr(slash + 1));
  return {std::stoi(values[0]), std::stoi(values[1]), fps > 0 ? fps : 30.0};
}

bool readFrame(FILE* input, std::vector<unsigned char>& bytes) {
  std::size_t read = 0;
  while (read < bytes.size()) {
    const std::size_t count = fread(bytes.data() + read, 1, bytes.size() - read, input);
    if (count == 0) return false;
    read += count;
  }
  return true;
}

bool writeFrame(FILE* output, const std::vector<unsigned char>& bytes) {
  return fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size();
}

struct Detection {
  cv::Rect box;
  float confidence;
  int classId;
  int trackId = -1;
};

struct Track {
  int id;
  int classId;
  cv::Rect box;
  int missedFrames = 0;
  bool identityDecided = false;
  std::string identity;
};

float intersectionOverUnion(const cv::Rect& first, const cv::Rect& second) {
  const cv::Rect overlap = first & second;
  const float intersection = static_cast<float>(overlap.area());
  const float combined = static_cast<float>(first.area() + second.area()) - intersection;
  return combined <= 0 ? 0 : intersection / combined;
}

class ObjectTracker {
 public:
  void update(std::vector<Detection>& detections) {
    for (auto& track : tracks_) ++track.missedFrames;
    std::vector<bool> used(tracks_.size(), false);

    for (auto& detection : detections) {
      int bestTrack = -1;
      float bestIou = 0.25F;
      for (std::size_t index = 0; index < tracks_.size(); ++index) {
        const Track& track = tracks_[index];
        if (!used[index] && track.classId == detection.classId) {
          const float iou = intersectionOverUnion(track.box, detection.box);
          if (iou > bestIou) {
            bestIou = iou;
            bestTrack = static_cast<int>(index);
          }
        }
      }

      if (bestTrack >= 0) {
        Track& track = tracks_[bestTrack];
        track.box = detection.box;
        track.missedFrames = 0;
        detection.trackId = track.id;
        used[bestTrack] = true;
      } else {
        detection.trackId = nextId_++;
        tracks_.push_back(
            {detection.trackId, detection.classId, detection.box, 0, false, ""});
        used.push_back(true);
      }
    }

    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [](const Track& track) { return track.missedFrames > 15; }),
                  tracks_.end());
  }

  bool identityDecided(int trackId) const {
    const Track* track = findTrack(trackId);
    return track != nullptr && track->identityDecided;
  }

  void setIdentity(int trackId, const std::string& identity) {
    Track* track = findTrack(trackId);
    if (track == nullptr) return;
    track->identity = identity;
    track->identityDecided = true;
  }

  std::string identity(int trackId) const {
    const Track* track = findTrack(trackId);
    return track == nullptr ? "" : track->identity;
  }

 private:
  Track* findTrack(int trackId) {
    for (auto& track : tracks_) {
      if (track.id == trackId) return &track;
    }
    return nullptr;
  }

  const Track* findTrack(int trackId) const {
    for (const auto& track : tracks_) {
      if (track.id == trackId) return &track;
    }
    return nullptr;
  }

  int nextId_ = 1;
  std::vector<Track> tracks_;
};

struct KnownFace {
  std::string identity;
  cv::Mat feature;
};

class FaceRecognition {
 public:
  FaceRecognition(const fs::path& detectorModel, const fs::path& recognitionModel,
                  const fs::path& databaseDirectory) {
    if (!fs::exists(detectorModel) || !fs::exists(recognitionModel)) {
      std::cerr << "Face models missing. Run ./download_face_models.sh first.\n";
      return;
    }
    detector_ = cv::FaceDetectorYN::create(detectorModel.string(), "", cv::Size(320, 320),
                                            0.85F, 0.3F, 5000);
    recognizer_ = cv::FaceRecognizerSF::create(recognitionModel.string(), "");
    loadDatabase(databaseDirectory);
    enabled_ = true;
    std::cout << "Loaded " << knownFaces_.size() << " face samples from "
              << databaseDirectory << '\n';
  }

  void recognizeNewTracks(const cv::Mat& frame, const std::vector<Detection>& detections,
                          ObjectTracker& tracker) {
    if (!enabled_) return;
    detector_->setInputSize(frame.size());
    cv::Mat faces;
    detector_->detect(frame, faces);
    for (int row = 0; row < faces.rows; ++row) {
      const cv::Rect faceBox(static_cast<int>(faces.at<float>(row, 0)),
                             static_cast<int>(faces.at<float>(row, 1)),
                             static_cast<int>(faces.at<float>(row, 2)),
                             static_cast<int>(faces.at<float>(row, 3)));
      const cv::Point center(faceBox.x + faceBox.width / 2, faceBox.y + faceBox.height / 2);
      const Detection* person = nullptr;
      for (const Detection& detection : detections) {
        if (detection.classId == 0 && detection.box.contains(center)) {
          person = &detection;
          break;
        }
      }
      if (person == nullptr || tracker.identityDecided(person->trackId)) continue;

      cv::Mat alignedFace;
      cv::Mat feature;
      recognizer_->alignCrop(frame, faces.row(row), alignedFace);
      recognizer_->feature(alignedFace, feature);
      const auto [identity, score] = bestMatch(feature);
      if (score >= kRecognizedFaceThreshold) {
        tracker.setIdentity(person->trackId, identity);
      } else if (knownFaces_.empty() || score < kUnrecognizedFaceThreshold) {
        tracker.setIdentity(person->trackId, "Unrecognized");
      }
    }
  }

 private:
  void loadDatabase(const fs::path& directory) {
    if (!fs::is_directory(directory)) return;
    for (const auto& identityDirectory : fs::directory_iterator(directory)) {
      if (!identityDirectory.is_directory()) continue;
      for (const auto& image : fs::directory_iterator(identityDirectory.path())) {
        if (!image.is_regular_file()) continue;
        cv::Mat sample = cv::imread(image.path().string());
        if (sample.empty()) continue;
        const auto feature = extractFeature(sample);
        if (feature) knownFaces_.push_back({identityDirectory.path().filename().string(), *feature});
      }
    }
  }

  std::optional<cv::Mat> extractFeature(const cv::Mat& image) {
    detector_->setInputSize(image.size());
    cv::Mat faces;
    detector_->detect(image, faces);
    if (faces.rows == 0) return std::nullopt;
    int bestFace = 0;
    for (int row = 1; row < faces.rows; ++row) {
      if (faces.at<float>(row, 14) > faces.at<float>(bestFace, 14)) bestFace = row;
    }
    cv::Mat alignedFace;
    cv::Mat feature;
    recognizer_->alignCrop(image, faces.row(bestFace), alignedFace);
    recognizer_->feature(alignedFace, feature);
    return feature;
  }

  std::pair<std::string, double> bestMatch(const cv::Mat& feature) const {
    std::string identity;
    double bestScore = -1;
    for (const KnownFace& known : knownFaces_) {
      const double score = recognizer_->match(feature, known.feature,
                                               cv::FaceRecognizerSF::FR_COSINE);
      if (score > bestScore) {
        bestScore = score;
        identity = known.identity;
      }
    }
    return {identity, bestScore};
  }

  bool enabled_ = false;
  cv::Ptr<cv::FaceDetectorYN> detector_;
  cv::Ptr<cv::FaceRecognizerSF> recognizer_;
  std::vector<KnownFace> knownFaces_;
};

void drawDetections(cv::Mat& frame, cv::dnn::Net& net, ObjectTracker& tracker,
                    FaceRecognition& faceRecognition) {
  const float xScale = static_cast<float>(frame.cols) / kInputSize;
  const float yScale = static_cast<float>(frame.rows) / kInputSize;
  const cv::Mat blob = cv::dnn::blobFromImage(
      frame, 1.0 / 255.0, cv::Size(kInputSize, kInputSize), cv::Scalar(), true, false);
  net.setInput(blob);
  const cv::Mat output = net.forward();  // YOLOv5 ONNX: [1, 25200, 85]

  if (output.dims != 3 || output.size[2] != 85) {
    throw std::runtime_error("Unexpected YOLO output. Use models/yolov5n.onnx.");
  }

  const cv::Mat predictions(output.size[1], output.size[2], CV_32F,
                            const_cast<float*>(output.ptr<float>()));
  std::vector<cv::Rect> boxes;
  std::vector<float> confidences;
  std::vector<int> classIds;

  for (int row = 0; row < predictions.rows; ++row) {
    const float* prediction = predictions.ptr<float>(row);
    const float objectness = prediction[4];
    if (objectness < kConfidenceThreshold) continue;

    cv::Mat scores(1, static_cast<int>(kCocoClasses.size()), CV_32F,
                   const_cast<float*>(prediction + 5));
    cv::Point classIdPoint;
    double classScore;
    cv::minMaxLoc(scores, nullptr, &classScore, nullptr, &classIdPoint);
    const float confidence = objectness * static_cast<float>(classScore);
    if (confidence < kConfidenceThreshold) continue;

    const float centerX = prediction[0] * xScale;
    const float centerY = prediction[1] * yScale;
    const float width = prediction[2] * xScale;
    const float height = prediction[3] * yScale;
    boxes.emplace_back(static_cast<int>(centerX - width / 2),
                       static_cast<int>(centerY - height / 2),
                       static_cast<int>(width), static_cast<int>(height));
    confidences.push_back(confidence);
    classIds.push_back(classIdPoint.x);
  }

  std::vector<int> kept;
  cv::dnn::NMSBoxes(boxes, confidences, kConfidenceThreshold, kNmsThreshold, kept);
  std::vector<Detection> detections;
  for (int index : kept) {
    const cv::Rect box = boxes[index] & cv::Rect(0, 0, frame.cols, frame.rows);
    if (box.area() > 0) detections.push_back({box, confidences[index], classIds[index]});
  }
  tracker.update(detections);
  faceRecognition.recognizeNewTracks(frame, detections, tracker);

  for (const Detection& detection : detections) {
    const std::string identity = tracker.identity(detection.trackId);
    const bool isUnrecognized = identity == "Unrecognized";
    const std::string label = (identity.empty() ? kCocoClasses[detection.classId] : identity) +
                              " #" + std::to_string(detection.trackId) + " " +
                              cv::format("%.2f", detection.confidence);
    const cv::Scalar color = isUnrecognized ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
    cv::rectangle(frame, detection.box, color, 2);
    cv::putText(frame, label,
                cv::Point(detection.box.x, std::max(20, detection.box.y - 6)),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
  }
}

}  // namespace

int main() {
  const fs::path inputVideo = findInputVideo();
  const fs::path model = "models/yolov5n.onnx";
  const fs::path faceDetectorModel = "models/face_detection_yunet_2023mar.onnx";
  const fs::path faceRecognitionModel = "models/face_recognition_sface_2021dec.onnx";
  const fs::path faceDatabase = fs::is_directory("face_database")
                                    ? fs::path("face_database")
                                    : fs::path("../server/face_database");
  const fs::path outputDirectory = "output";
  const fs::path outputVideo = outputDirectory / "test_detected.mp4";

  if (inputVideo.empty()) {
    std::cerr << "No input found. Add exactly one file named source/test.<video-extension>.\n";
    return 1;
  }
  if (!fs::exists(model)) {
    std::cerr << "Model missing: " << model << ". Run ./download_model.sh first.\n";
    return 1;
  }
  fs::create_directories(outputDirectory);

  try {
    const VideoInfo info = probeVideo(inputVideo);
    const std::string decoderCommand =
        "ffmpeg -v error -i " + shellQuote(inputVideo.string()) +
        " -map 0:v:0 -f rawvideo -pix_fmt bgr24 -";
    const std::string encoderCommand =
        "ffmpeg -y -v error -f rawvideo -pixel_format bgr24 -video_size " +
        std::to_string(info.width) + "x" + std::to_string(info.height) +
        " -framerate " + std::to_string(info.fps) +
        " -i - -an -c:v libx264 -pix_fmt yuv420p " + shellQuote(outputVideo.string());
    FILE* decoder = popen(decoderCommand.c_str(), "r");
    FILE* encoder = popen(encoderCommand.c_str(), "w");
    if (decoder == nullptr || encoder == nullptr) {
      if (decoder != nullptr) pclose(decoder);
      if (encoder != nullptr) pclose(encoder);
      throw std::runtime_error("Unable to start FFmpeg.");
    }

    cv::dnn::Net net = cv::dnn::readNetFromONNX(model.string());
    ObjectTracker tracker;
    FaceRecognition faceRecognition(faceDetectorModel, faceRecognitionModel, faceDatabase);
    std::vector<unsigned char> bytes(
        static_cast<std::size_t>(info.width) * info.height * 3);
    std::size_t frameCount = 0;
    while (readFrame(decoder, bytes)) {
      cv::Mat frame(info.height, info.width, CV_8UC3, bytes.data());
      drawDetections(frame, net, tracker, faceRecognition);
      if (!writeFrame(encoder, bytes)) throw std::runtime_error("Unable to write output frame.");
      ++frameCount;
      if (frameCount % 25 == 0) {
        std::cout << "Processed " << frameCount << " frames..." << std::endl;
      }
    }
    const int decoderStatus = pclose(decoder);
    const int encoderStatus = pclose(encoder);
    if (decoderStatus != 0 || encoderStatus != 0) {
      throw std::runtime_error("FFmpeg failed while processing the video.");
    }
    std::cout << "Processed " << frameCount << " frames. Output: " << outputVideo << '\n';
  } catch (const std::exception& error) {
    std::cerr << "Processing failed: " << error.what() << '\n';
    return 1;
  }
}
