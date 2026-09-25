#include "video_stream_server.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kVideoPort = 5001;
constexpr char kProcessorSocketPath[] = "/tmp/whats_that_processor.sock";
constexpr std::uint32_t kMaxFrameBytes = 10 * 1024 * 1024;
constexpr std::size_t kSessionIdBytes = 32;
constexpr size_t kUdpChunkPayloadSize = 1300;
constexpr size_t kUdpHeaderSize = 46;

std::mutex processorMutex;
int processorFd = -1;
std::mutex subscribersMutex;
std::unordered_map<std::string, std::vector<int>> subscribers;

// UDP client addressing and state
std::mutex udpClientsMutex;
std::unordered_map<std::string, sockaddr_in> udpClients;
int globalUdpFd = -1;
std::atomic<uint32_t> udpOutgoingFrameId{0};

// Low-latency queue between UDP reception and local processor
std::mutex pendingFrameMutex;
std::condition_variable pendingFrameCv;
std::string pendingSessionId;
std::vector<uint8_t> pendingFrame;
bool hasPendingFrame = false;
bool processorBusy = false;

struct FrameSlot {
  uint32_t frameId = 0;
  uint16_t totalChunks = 0;
  uint16_t receivedCount = 0;
  std::chrono::steady_clock::time_point createdAt;
  std::vector<std::vector<uint8_t>> chunks;
};

class UdpFrameAssembly {
 public:
  bool addChunk(uint32_t incomingFrameId, uint16_t chunkIndex, uint16_t incomingTotalChunks,
                const uint8_t* payload, uint16_t payloadLen, std::vector<uint8_t>& outFullFrame) {
    if (incomingTotalChunks == 0 || chunkIndex >= incomingTotalChunks || payloadLen == 0) return false;

    const auto now = std::chrono::steady_clock::now();

    // Clean up stale slots older than 1.5 seconds
    for (auto it = slots_.begin(); it != slots_.end();) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.createdAt).count() > 1500) {
        it = slots_.erase(it);
      } else {
        ++it;
      }
    }

    if (lastCompletedFrameId_ > 0 && incomingFrameId <= lastCompletedFrameId_) {
      if (lastCompletedFrameId_ - incomingFrameId > 50) {
        // Stream restarted or frame counter reset: restart assembly tracking
        lastCompletedFrameId_ = 0;
        slots_.clear();
      } else {
        // Late duplicate chunk for older already completed frame
        return false;
      }
    }

    if (slots_.find(incomingFrameId) == slots_.end()) {
      if (slots_.size() >= 8) {
        uint32_t oldestId = slots_.begin()->first;
        for (const auto& kv : slots_) {
          if (kv.first < oldestId) oldestId = kv.first;
        }
        slots_.erase(oldestId);
      }
    }

    auto& slot = slots_[incomingFrameId];
    if (slot.chunks.empty()) {
      slot.frameId = incomingFrameId;
      slot.totalChunks = incomingTotalChunks;
      slot.receivedCount = 0;
      slot.createdAt = now;
      slot.chunks.resize(incomingTotalChunks);
    } else if (slot.totalChunks != incomingTotalChunks) {
      return false;
    }

    if (slot.chunks[chunkIndex].empty()) {
      slot.chunks[chunkIndex].assign(payload, payload + payloadLen);
      ++slot.receivedCount;
    }

    if (slot.receivedCount == slot.totalChunks) {
      size_t totalBytes = 0;
      for (const auto& c : slot.chunks) totalBytes += c.size();
      outFullFrame.clear();
      outFullFrame.reserve(totalBytes);
      for (const auto& c : slot.chunks) {
        outFullFrame.insert(outFullFrame.end(), c.begin(), c.end());
      }
      lastCompletedFrameId_ = incomingFrameId;
      slots_.erase(incomingFrameId);
      for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->first <= incomingFrameId) {
          it = slots_.erase(it);
        } else {
          ++it;
        }
      }
      return true;
    }
    return false;
  }

 private:
  uint32_t lastCompletedFrameId_ = 0;
  std::unordered_map<uint32_t, FrameSlot> slots_;
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

std::array<std::uint8_t, 12> makeHeader(const char magic[4], std::uint32_t ipLength,
                                        std::uint32_t frameLength) {
  std::array<std::uint8_t, 12> header{};
  std::memcpy(header.data(), magic, 4);
  const std::uint32_t networkIpLength = htonl(ipLength);
  const std::uint32_t networkFrameLength = htonl(frameLength);
  std::memcpy(header.data() + 4, &networkIpLength, sizeof(networkIpLength));
  std::memcpy(header.data() + 8, &networkFrameLength, sizeof(networkFrameLength));
  return header;
}

void sendUdpChunks(int fd, const sockaddr_in& targetAddress, const std::string& sessionId,
                   uint32_t frameId, const std::vector<uint8_t>& frame) {
  const uint16_t totalChunks =
      static_cast<uint16_t>((frame.size() + kUdpChunkPayloadSize - 1) / kUdpChunkPayloadSize);
  for (uint16_t chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex) {
    const size_t offset = chunkIndex * kUdpChunkPayloadSize;
    const size_t len = std::min(kUdpChunkPayloadSize, frame.size() - offset);
    std::vector<uint8_t> packet(kUdpHeaderSize + len);
    packet[0] = 'U';
    packet[1] = 'D';
    packet[2] = 'P';
    packet[3] = '1';
    std::memcpy(packet.data() + 4, sessionId.data(), std::min(sessionId.size(), size_t(32)));
    const uint32_t netFrameId = htonl(frameId);
    const uint16_t netChunkIndex = htons(chunkIndex);
    const uint16_t netTotalChunks = htons(totalChunks);
    const uint16_t netPayloadLen = htons(static_cast<uint16_t>(len));
    std::memcpy(packet.data() + 36, &netFrameId, 4);
    std::memcpy(packet.data() + 40, &netChunkIndex, 2);
    std::memcpy(packet.data() + 42, &netTotalChunks, 2);
    std::memcpy(packet.data() + 44, &netPayloadLen, 2);
    std::memcpy(packet.data() + kUdpHeaderSize, frame.data() + offset, len);
    sendto(fd, packet.data(), packet.size(), 0,
           reinterpret_cast<const sockaddr*>(&targetAddress), sizeof(targetAddress));
    if (chunkIndex % 4 == 3) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

std::string getClientIp(const sockaddr_in& address) {
  char ip[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) return "unknown";
  return ip;
}

void sendFrameToPhone(const std::string& sessionId, const std::vector<std::uint8_t>& frame) {
  // 1. Send via low-latency UDP
  sockaddr_in clientAddr{};
  bool hasUdpClient = false;
  {
    std::lock_guard<std::mutex> lock(udpClientsMutex);
    const auto found = udpClients.find(sessionId);
    if (found != udpClients.end()) {
      clientAddr = found->second;
      hasUdpClient = true;
    } else if (!udpClients.empty()) {
      clientAddr = udpClients.begin()->second;
      hasUdpClient = true;
    }
  }
  if (hasUdpClient && globalUdpFd != -1) {
    const uint32_t frameId = ++udpOutgoingFrameId;
    sendUdpChunks(globalUdpFd, clientAddr, sessionId, frameId, frame);
    if (frameId == 1 || frameId % 10 == 0) {
      std::cout << "Live UDP video: processed frame " << frameId << " sent to " << getClientIp(clientAddr) << std::endl;
    }
  }

  // 2. Also send via TCP if any legacy TCP subscribers exist
  {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    const auto found = subscribers.find(sessionId);
    if (found != subscribers.end()) {
      std::array<std::uint8_t, 16> header{};
      std::memcpy(header.data(), "FRM1", 4);
      const std::uint32_t networkLength = htonl(static_cast<std::uint32_t>(frame.size()));
      std::memcpy(header.data() + 12, &networkLength, sizeof(networkLength));
      for (auto iterator = found->second.begin(); iterator != found->second.end();) {
        if (!sendAll(*iterator, header.data(), header.size()) ||
            !sendAll(*iterator, frame.data(), frame.size())) {
          close(*iterator);
          iterator = found->second.erase(iterator);
        } else {
          ++iterator;
        }
      }
      if (found->second.empty()) subscribers.erase(found);
    }
  }
}

void forwardToProcessor(const std::string& sessionId, const std::vector<std::uint8_t>& frame) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(processorMutex);
    fd = processorFd;
  }
  if (fd == -1) {
    std::lock_guard<std::mutex> pLock(pendingFrameMutex);
    processorBusy = false;
    return;
  }
  const auto header = makeHeader("INP1", static_cast<std::uint32_t>(sessionId.size()),
                                 static_cast<std::uint32_t>(frame.size()));
  if (!sendAll(fd, header.data(), header.size()) ||
      !sendAll(fd, sessionId.data(), sessionId.size()) ||
      !sendAll(fd, frame.data(), frame.size())) {
    {
      std::lock_guard<std::mutex> lock(processorMutex);
      if (processorFd == fd) {
        close(processorFd);
        processorFd = -1;
      }
    }
    {
      std::lock_guard<std::mutex> pLock(pendingFrameMutex);
      processorBusy = false;
    }
    std::cerr << "Local processor disconnected" << std::endl;
  }
}

void queueFrameForProcessor(const std::string& sessionId, std::vector<uint8_t> frame) {
  {
    std::lock_guard<std::mutex> lock(pendingFrameMutex);
    pendingSessionId = sessionId;
    pendingFrame = std::move(frame);
    hasPendingFrame = true;
  }
  pendingFrameCv.notify_all();
}

void runProcessorDispatcher() {
  while (true) {
    std::string sessionId;
    std::vector<uint8_t> frame;
    {
      std::unique_lock<std::mutex> lock(pendingFrameMutex);
      pendingFrameCv.wait_for(lock, std::chrono::milliseconds(50), [&] {
        bool connected = false;
        {
          std::lock_guard<std::mutex> pLock(processorMutex);
          connected = (processorFd != -1);
        }
        return hasPendingFrame && !processorBusy && connected;
      });

      bool connected = false;
      {
        std::lock_guard<std::mutex> pLock(processorMutex);
        connected = (processorFd != -1);
      }

      if (!connected) {
        if (hasPendingFrame) {
          static auto lastWarn = std::chrono::steady_clock::now();
          const auto now = std::chrono::steady_clock::now();
          if (std::chrono::duration_cast<std::chrono::seconds>(now - lastWarn).count() >= 3) {
            std::cerr << "[WARNING] Frames are arriving from phone, but yolo_video_detector (server_side_process) is NOT connected! Please run ./build/yolo_video_detector in another terminal." << std::endl;
            lastWarn = now;
          }
        }
        processorBusy = false;
        continue;
      }

      if (processorBusy) {
        // Processor is still running inference on previous frame; wait for OUT1
        continue;
      }

      if (!hasPendingFrame) {
        continue;
      }

      sessionId = std::move(pendingSessionId);
      frame = std::move(pendingFrame);
      hasPendingFrame = false;
      processorBusy = true;
    }
    forwardToProcessor(sessionId, frame);
  }
}

void handleSubscriber(int clientFd, const std::string& sessionId) {
  int flag = 1;
  setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  int bufSize = 2 * 1024 * 1024;
  setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

  {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    subscribers[sessionId].push_back(clientFd);
  }
  std::cout << "[TCP] Subscriber registered for session: " << sessionId.substr(0, 8) << "..." << std::endl;
  char unused;
  while (recv(clientFd, &unused, 1, 0) > 0) {
  }
  {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    const auto found = subscribers.find(sessionId);
    if (found != subscribers.end()) {
      auto& entries = found->second;
      entries.erase(std::remove(entries.begin(), entries.end(), clientFd), entries.end());
      if (entries.empty()) subscribers.erase(found);
    }
  }
  close(clientFd);
  std::cout << "[TCP] Subscriber disconnected for session: " << sessionId.substr(0, 8) << "..." << std::endl;
}

void handleVideoClient(int clientFd, const std::string& clientIp,
                       const std::string& sessionId) {
  int flag = 1;
  setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  int bufSize = 2 * 1024 * 1024;
  setsockopt(clientFd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));

  std::cout << "[TCP] Live video stream started from: " << clientIp << std::endl;
  std::size_t frames = 0;
  while (true) {
    std::array<std::uint8_t, 16> header{};
    if (!receiveAll(clientFd, header.data(), header.size())) break;
    if (std::memcmp(header.data(), "FRM1", 4) != 0) {
      std::cerr << "[TCP] Invalid frame header from " << clientIp << std::endl;
      break;
    }
    std::uint32_t networkLength;
    std::memcpy(&networkLength, header.data() + 12, sizeof(networkLength));
    const std::uint32_t frameLength = ntohl(networkLength);
    if (frameLength == 0 || frameLength > kMaxFrameBytes) break;
    std::vector<std::uint8_t> frame(frameLength);
    if (!receiveAll(clientFd, frame.data(), frame.size())) break;
    if (frameLength >= 4 && frame[0] == 0xFF && frame[1] == 0xD8) {
      queueFrameForProcessor(sessionId, std::move(frame));
      ++frames;
      if (frames == 1 || frames % 10 == 0) {
        std::cout << "[TCP] Frame #" << frames << " (" << frameLength
                  << " bytes) received from " << clientIp << " -> forwarded to processor" << std::endl;
      }
    } else {
      std::cerr << "[TCP] Frame from " << clientIp << " has invalid JPEG magic: "
                << std::hex << (int)frame[0] << " " << (int)frame[1] << std::dec << std::endl;
    }
  }
  std::cout << "[TCP] Live video stream stopped: " << clientIp << " (total: " << frames << " frames)" << std::endl;
  close(clientFd);
}

void handleProcessor(int clientFd) {
  int bufSize = 4 * 1024 * 1024;
  setsockopt(clientFd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
  setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

  {
    std::lock_guard<std::mutex> lock(processorMutex);
    if (processorFd != -1) close(processorFd);
    processorFd = clientFd;
  }
  {
    std::lock_guard<std::mutex> lock(pendingFrameMutex);
    processorBusy = false;
  }
  pendingFrameCv.notify_all();
  std::cout << "Local server_side_process connected via internal IPC" << std::endl;
  while (true) {
    std::array<std::uint8_t, 12> header{};
    if (!receiveAll(clientFd, header.data(), header.size()) ||
        std::memcmp(header.data(), "OUT1", 4) != 0) break;
    std::uint32_t networkIpLength;
    std::uint32_t networkFrameLength;
    std::memcpy(&networkIpLength, header.data() + 4, sizeof(networkIpLength));
    std::memcpy(&networkFrameLength, header.data() + 8, sizeof(networkFrameLength));
    const std::uint32_t sessionIdLength = ntohl(networkIpLength);
    const std::uint32_t frameLength = ntohl(networkFrameLength);
    if (sessionIdLength != kSessionIdBytes || frameLength == 0 || frameLength > kMaxFrameBytes) break;
    std::string sessionId(sessionIdLength, '\0');
    std::vector<std::uint8_t> frame(frameLength);
    if (!receiveAll(clientFd, sessionId.data(), sessionId.size()) ||
        !receiveAll(clientFd, frame.data(), frame.size())) break;
    sendFrameToPhone(sessionId, frame);
    {
      std::lock_guard<std::mutex> lock(pendingFrameMutex);
      processorBusy = false;
    }
    pendingFrameCv.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(processorMutex);
    if (processorFd == clientFd) processorFd = -1;
  }
  {
    std::lock_guard<std::mutex> lock(pendingFrameMutex);
    processorBusy = false;
  }
  pendingFrameCv.notify_all();
  close(clientFd);
  std::cerr << "Local processor disconnected" << std::endl;
}

void runProcessorBridge() {
  std::vector<int> listenFds;

  // 1. Abstract Unix domain socket (always works, immune to stale files and root/user permissions)
  const int abstractFd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (abstractFd != -1) {
    sockaddr_un abstractAddr{};
    abstractAddr.sun_family = AF_UNIX;
    abstractAddr.sun_path[0] = '\0';
    std::strncpy(abstractAddr.sun_path + 1, "whats_that_processor", sizeof(abstractAddr.sun_path) - 2);
    const socklen_t abstractLen = sizeof(sa_family_t) + 1 + std::strlen("whats_that_processor");
    if (bind(abstractFd, reinterpret_cast<sockaddr*>(&abstractAddr), abstractLen) == 0 &&
        listen(abstractFd, 5) == 0) {
      listenFds.push_back(abstractFd);
    } else {
      close(abstractFd);
    }
  }

  // 2. Filesystem socket (for backwards compatibility if permissions permit)
  const int fileFd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fileFd != -1) {
    unlink(kProcessorSocketPath);
    sockaddr_un fileAddr{};
    fileAddr.sun_family = AF_UNIX;
    std::strncpy(fileAddr.sun_path, kProcessorSocketPath, sizeof(fileAddr.sun_path) - 1);
    if (bind(fileFd, reinterpret_cast<sockaddr*>(&fileAddr), sizeof(fileAddr)) == 0 &&
        listen(fileFd, 5) == 0) {
      chmod(kProcessorSocketPath, 0666);
      listenFds.push_back(fileFd);
    } else {
      close(fileFd);
    }
  }

  if (listenFds.empty()) {
    std::cerr << "Local processor bridge could not listen on any socket." << std::endl;
    return;
  }

  std::cout << "Waiting for local server_side_process" << std::endl;
  while (true) {
    std::vector<pollfd> pFds;
    for (int fd : listenFds) pFds.push_back({fd, POLLIN, 0});
    if (poll(pFds.data(), pFds.size(), -1) <= 0) continue;
    for (const auto& pfd : pFds) {
      if (pfd.revents & POLLIN) {
        const int clientFd = accept(pfd.fd, nullptr, nullptr);
        if (clientFd != -1) std::thread(handleProcessor, clientFd).detach();
      }
    }
  }
}

void runUdpServer() {
  const int udpFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (udpFd == -1) {
    std::cerr << "UDP socket creation failed: " << std::strerror(errno) << std::endl;
    return;
  }
  int reuse = 1;
  setsockopt(udpFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int rcvBufSize = 2 * 1024 * 1024;
  setsockopt(udpFd, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, sizeof(rcvBufSize));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(kVideoPort);

  if (bind(udpFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
    std::cerr << "UDP bind failed on port " << kVideoPort << ": " << std::strerror(errno) << std::endl;
    close(udpFd);
    return;
  }

  globalUdpFd = udpFd;
  std::cout << "Live UDP video stream listening on 0.0.0.0:" << kVideoPort << std::endl;

  std::unordered_map<std::string, UdpFrameAssembly> assemblers;
  std::vector<uint8_t> buffer(65536);
  std::size_t udpFrameCount = 0;
  std::size_t rawUdpPackets = 0;

  while (true) {
    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
    const ssize_t received = recvfrom(udpFd, buffer.data(), buffer.size(), 0,
                                      reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (received < static_cast<ssize_t>(kUdpHeaderSize)) continue;

    if (std::memcmp(buffer.data(), "UDP1", 4) != 0) continue;

    ++rawUdpPackets;
    if (rawUdpPackets == 1 || rawUdpPackets % 20 == 0) {
      std::cout << "[UDP] Received packet #" << rawUdpPackets << " (" << received
                << " bytes) from " << getClientIp(clientAddr) << ":"
                << ntohs(clientAddr.sin_port) << std::endl;
    }

    std::string sessionId(reinterpret_cast<const char*>(buffer.data() + 4), 32);
    {
      std::lock_guard<std::mutex> lock(udpClientsMutex);
      udpClients[sessionId] = clientAddr;
    }

    uint32_t netFrameId;
    uint16_t netChunkIndex, netTotalChunks, netPayloadLen;
    std::memcpy(&netFrameId, buffer.data() + 36, 4);
    std::memcpy(&netChunkIndex, buffer.data() + 40, 2);
    std::memcpy(&netTotalChunks, buffer.data() + 42, 2);
    std::memcpy(&netPayloadLen, buffer.data() + 44, 2);

    const uint32_t frameId = ntohl(netFrameId);
    const uint16_t chunkIndex = ntohs(netChunkIndex);
    const uint16_t totalChunks = ntohs(netTotalChunks);
    const uint16_t payloadLen = ntohs(netPayloadLen);

    if (payloadLen == 0 || totalChunks == 0) continue;
    if (received < static_cast<ssize_t>(kUdpHeaderSize + payloadLen)) continue;

    std::vector<uint8_t> fullFrame;
    if (assemblers[sessionId].addChunk(frameId, chunkIndex, totalChunks,
                                       buffer.data() + kUdpHeaderSize, payloadLen, fullFrame)) {
      if (fullFrame.size() >= 4 && fullFrame[0] == 0xFF && fullFrame[1] == 0xD8) {
        const size_t frameSize = fullFrame.size();
        queueFrameForProcessor(sessionId, std::move(fullFrame));
        ++udpFrameCount;
        if (udpFrameCount == 1 || udpFrameCount % 10 == 0) {
          std::cout << "[UDP] Live video frame #" << udpFrameCount << " (" << frameSize
                    << " bytes) forwarded to processor" << std::endl;
        }
      } else {
        std::cerr << "[UDP] Frame #" << frameId << " has invalid JPEG header: "
                  << std::hex << (int)fullFrame[0] << " " << (int)fullFrame[1] << std::dec << std::endl;
      }
    }
  }
}

}  // namespace

void runVideoStreamServer() {
  std::thread(runProcessorBridge).detach();
  std::thread(runProcessorDispatcher).detach();
  std::thread(runUdpServer).detach();

  const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd == -1) return;
  int reuseAddress = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(kVideoPort);
  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
      listen(serverFd, 5) == -1) {
    std::cerr << "Video stream TCP server could not listen on port 5001: " << std::strerror(errno) << '\n';
    close(serverFd);
    return;
  }
  std::cout << "Live video TCP server listening on 0.0.0.0:5001" << std::endl;
  while (true) {
    sockaddr_in clientAddress{};
    socklen_t length = sizeof(clientAddress);
    const int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &length);
    if (clientFd == -1) continue;
    const std::string clientIp = getClientIp(clientAddress);
    std::array<std::uint8_t, 4> firstBytes{};
    if (!receiveAll(clientFd, firstBytes.data(), firstBytes.size())) {
      close(clientFd);
      continue;
    }
    if (std::memcmp(firstBytes.data(), "CHK1", 4) == 0) {
      std::array<char, kSessionIdBytes> sessionBytes{};
      receiveAll(clientFd, sessionBytes.data(), sessionBytes.size());
      sendAll(clientFd, "OK\n", 3);
      std::cout << "[TCP] Connection check passed from: " << clientIp << std::endl;
      close(clientFd);
    } else if (std::memcmp(firstBytes.data(), "SUB1", 4) == 0) {
      std::array<char, kSessionIdBytes> sessionBytes{};
      if (!receiveAll(clientFd, sessionBytes.data(), sessionBytes.size())) {
        close(clientFd);
        continue;
      }
      std::thread(handleSubscriber, clientFd,
                  std::string(sessionBytes.data(), sessionBytes.size())).detach();
    } else if (std::memcmp(firstBytes.data(), "STR1", 4) == 0) {
      std::array<char, kSessionIdBytes> sessionBytes{};
      if (!receiveAll(clientFd, sessionBytes.data(), sessionBytes.size())) {
        close(clientFd);
        continue;
      }
      sendAll(clientFd, "OK\n", 3);
      std::thread(handleVideoClient, clientFd, clientIp,
                  std::string(sessionBytes.data(), sessionBytes.size())).detach();
    } else {
      close(clientFd);
    }
  }
}
