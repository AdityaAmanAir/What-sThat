#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
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
constexpr char kProcessorSocketPath[] = "/tmp/whats_that_processor.sock";
constexpr std::uint32_t kMaxLiveFrameBytes = 10 * 1024 * 1024;

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

bool receiveAll(int fd, void* destination, std::size_t length) {
  auto* bytes = static_cast<std::uint8_t*>(destination);
  std::size_t received = 0;
  while (received < length) {
    const ssize_t result = recv(fd, bytes + received, length - received, 0);
    if (result <= 0) return false;
    received += static_cast<std::size_t>(result);
  }
  return true;
}

bool sendAll(int fd, const void* source, std::size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(source);
  std::size_t sent = 0;
  while (sent < length) {
    const ssize_t result = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
    if (result <= 0) return false;
    sent += static_cast<std::size_t>(result);
  }
  return true;
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
  fs::path model = "models/yolov5n.onnx";
  if (!fs::exists(model)) model = "server_side_process/models/yolov5n.onnx";
  if (!fs::exists(model)) model = "../server_side_process/models/yolov5n.onnx";

  fs::path faceDetectorModel = "models/face_detection_yunet_2023mar.onnx";
  if (!fs::exists(faceDetectorModel)) faceDetectorModel = "server_side_process/models/face_detection_yunet_2023mar.onnx";
  if (!fs::exists(faceDetectorModel)) faceDetectorModel = "../server_side_process/models/face_detection_yunet_2023mar.onnx";

  fs::path faceRecognitionModel = "models/face_recognition_sface_2021dec.onnx";
  if (!fs::exists(faceRecognitionModel)) faceRecognitionModel = "server_side_process/models/face_recognition_sface_2021dec.onnx";
  if (!fs::exists(faceRecognitionModel)) faceRecognitionModel = "../server_side_process/models/face_recognition_sface_2021dec.onnx";

  fs::path faceDatabase = fs::is_directory("face_database")
                              ? fs::path("face_database")
                              : (fs::is_directory("../server/face_database")
                                     ? fs::path("../server/face_database")
                                     : fs::path("server/face_database"));
  if (!fs::exists(model)) {
    std::cerr << "Model missing: " << model << ". Run ./download_model.sh first.\n";
    return 1;
  }
  try {
    const unsigned int cpuCores = std::max(1U, std::thread::hardware_concurrency());
    cv::setUseOptimized(true);
    cv::setNumThreads(static_cast<int>(cpuCores));
    std::cout << "OpenCV CPU processing configured for " << cv::getNumThreads()
              << " worker threads." << std::endl;

    cv::dnn::Net net = cv::dnn::readNetFromONNX(model.string());
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    FaceRecognition faceRecognition(faceDetectorModel, faceRecognitionModel, faceDatabase);

    std::unordered_map<std::string, ObjectTracker> trackers;

    sockaddr_un abstractAddr{};
    abstractAddr.sun_family = AF_UNIX;
    abstractAddr.sun_path[0] = '\0';
    std::strncpy(abstractAddr.sun_path + 1, "whats_that_processor", sizeof(abstractAddr.sun_path) - 2);
    const socklen_t abstractLen = sizeof(sa_family_t) + 1 + std::strlen("whats_that_processor");

    sockaddr_un fileAddr{};
    fileAddr.sun_family = AF_UNIX;
    std::strncpy(fileAddr.sun_path, kProcessorSocketPath, sizeof(fileAddr.sun_path) - 1);

    while (true) {
      int processorFd = -1;
      std::cout << "Waiting for local server bridge via internal IPC..." << std::endl;
      while (processorFd == -1) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd != -1) {
          int bufSize = 4 * 1024 * 1024;
          setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
          setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

          if (connect(fd, reinterpret_cast<sockaddr*>(&abstractAddr), abstractLen) == 0) {
            processorFd = fd;
            break;
          }
          if (connect(fd, reinterpret_cast<sockaddr*>(&fileAddr), sizeof(fileAddr)) == 0) {
            processorFd = fd;
            break;
          }
          close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      std::cout << "Connected to local server bridge via internal IPC. Ready for frames!" << std::endl;
      std::size_t frameCount = 0;

      while (true) {
        std::array<std::uint8_t, 12> header{};
        if (!receiveAll(processorFd, header.data(), header.size()) ||
            std::memcmp(header.data(), "INP1", 4) != 0) break;
        std::uint32_t networkIpLength;
        std::uint32_t networkFrameLength;
        std::memcpy(&networkIpLength, header.data() + 4, sizeof(networkIpLength));
        std::memcpy(&networkFrameLength, header.data() + 8, sizeof(networkFrameLength));
        const std::uint32_t sessionIdLength = ntohl(networkIpLength);
        const std::uint32_t frameLength = ntohl(networkFrameLength);
        if (sessionIdLength != 32 || frameLength == 0 || frameLength > kMaxLiveFrameBytes) break;
        std::string sessionId(sessionIdLength, '\0');
        std::vector<std::uint8_t> encoded(frameLength);
        if (!receiveAll(processorFd, sessionId.data(), sessionId.size()) ||
            !receiveAll(processorFd, encoded.data(), encoded.size())) break;

        cv::Mat frame = cv::imdecode(encoded, cv::IMREAD_COLOR);
        std::vector<std::uint8_t> processed;
        if (!frame.empty()) {
          drawDetections(frame, net, trackers[sessionId], faceRecognition);
          cv::imencode(".jpg", frame, processed, {cv::IMWRITE_JPEG_QUALITY, 70});
        } else {
          std::cerr << "[PROCESSOR] WARNING: cv::imdecode failed to decode frame of "
                    << encoded.size() << " bytes!" << std::endl;
        }
        if (processed.empty()) {
          processed = std::move(encoded);
        }

        std::array<std::uint8_t, 12> outputHeader{};
        std::memcpy(outputHeader.data(), "OUT1", 4);
        const std::uint32_t networkOutputSessionLength = htonl(sessionIdLength);
        const std::uint32_t networkOutputFrameLength = htonl(static_cast<std::uint32_t>(processed.size()));
        std::memcpy(outputHeader.data() + 4, &networkOutputSessionLength,
                    sizeof(networkOutputSessionLength));
        std::memcpy(outputHeader.data() + 8, &networkOutputFrameLength, sizeof(networkOutputFrameLength));
        if (!sendAll(processorFd, outputHeader.data(), outputHeader.size()) ||
            !sendAll(processorFd, sessionId.data(), sessionId.size()) ||
            !sendAll(processorFd, processed.data(), processed.size())) break;
        ++frameCount;
        if (frameCount == 1 || frameCount % 10 == 0) {
          std::cout << "[PROCESSOR] Frame #" << frameCount << ": "
                    << (frame.empty() ? 0 : frame.cols) << "x" << (frame.empty() ? 0 : frame.rows)
                    << " -> processed & encoded " << processed.size() << " bytes." << std::endl;
        }
      }

      close(processorFd);
      std::cout << "Local server bridge disconnected after " << frameCount
                << " frames. Reconnecting..." << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  } catch (const std::exception& error) {
    std::cerr << "Processing failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
