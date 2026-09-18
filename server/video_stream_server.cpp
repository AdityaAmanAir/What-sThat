#include "video_stream_server.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kVideoPort = 5001;
constexpr char kProcessorSocketPath[] = "/tmp/whats_that_processor.sock";
constexpr std::uint32_t kMaxFrameBytes = 10 * 1024 * 1024;
constexpr std::uint32_t kMaxIpBytes = 64;
constexpr std::size_t kSessionIdBytes = 32;

std::mutex processorMutex;
int processorFd = -1;
std::mutex subscribersMutex;
std::unordered_map<std::string, std::vector<int>> subscribers;

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

void sendFrameToPhone(const std::string& sessionId, const std::vector<std::uint8_t>& frame) {
  std::array<std::uint8_t, 16> header{};
  std::memcpy(header.data(), "FRM1", 4);
  const std::uint32_t networkLength = htonl(static_cast<std::uint32_t>(frame.size()));
  std::memcpy(header.data() + 12, &networkLength, sizeof(networkLength));

  std::lock_guard<std::mutex> lock(subscribersMutex);
  const auto found = subscribers.find(sessionId);
  if (found == subscribers.end()) return;
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

void forwardToProcessor(const std::string& sessionId, const std::vector<std::uint8_t>& frame) {
  std::lock_guard<std::mutex> lock(processorMutex);
  if (processorFd == -1) return;
  const auto header = makeHeader("INP1", static_cast<std::uint32_t>(sessionId.size()),
                                 static_cast<std::uint32_t>(frame.size()));
  if (!sendAll(processorFd, header.data(), header.size()) ||
      !sendAll(processorFd, sessionId.data(), sessionId.size()) ||
      !sendAll(processorFd, frame.data(), frame.size())) {
    close(processorFd);
    processorFd = -1;
    std::cerr << "Local processor disconnected" << std::endl;
  }
}

void handleSubscriber(int clientFd, const std::string& sessionId) {
  {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    subscribers[sessionId].push_back(clientFd);
  }
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
}

void handleVideoClient(int clientFd, const std::string& clientIp,
                       const std::string& sessionId, std::array<std::uint8_t, 4> firstBytes) {
  std::size_t frames = 0;
  while (true) {
    std::array<std::uint8_t, 16> header{};
    std::memcpy(header.data(), firstBytes.data(), firstBytes.size());
    if (!receiveAll(clientFd, header.data() + 4, header.size() - 4) ||
        std::memcmp(header.data(), "FRM1", 4) != 0) break;
    std::uint32_t networkLength;
    std::memcpy(&networkLength, header.data() + 12, sizeof(networkLength));
    const std::uint32_t frameLength = ntohl(networkLength);
    if (frameLength == 0 || frameLength > kMaxFrameBytes) break;
    std::vector<std::uint8_t> frame(frameLength);
    if (!receiveAll(clientFd, frame.data(), frame.size())) break;
    if (frameLength >= 4 && frame[0] == 0xFF && frame[1] == 0xD8 &&
        frame[frameLength - 2] == 0xFF && frame[frameLength - 1] == 0xD9) {
      forwardToProcessor(sessionId, frame);
      ++frames;
      if (frames % 10 == 0) {
        std::cout << "Live video " << clientIp << ": " << frames << " frames forwarded" << std::endl;
      }
    }
    if (!receiveAll(clientFd, firstBytes.data(), firstBytes.size())) break;
  }
  std::cout << "Live video stopped: " << clientIp << std::endl;
  close(clientFd);
}

void handleProcessor(int clientFd) {
  {
    std::lock_guard<std::mutex> lock(processorMutex);
    if (processorFd != -1) close(processorFd);
    processorFd = clientFd;
  }
  std::cout << "Local server_side_process connected" << std::endl;
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
  }
  std::lock_guard<std::mutex> lock(processorMutex);
  if (processorFd == clientFd) processorFd = -1;
  close(clientFd);
  std::cerr << "Local processor disconnected" << std::endl;
}

void runProcessorBridge() {
  unlink(kProcessorSocketPath);
  const int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (serverFd == -1) return;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, kProcessorSocketPath, sizeof(address.sun_path) - 1);
  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
      listen(serverFd, 1) == -1) {
    std::cerr << "Local processor bridge failed: " << std::strerror(errno) << '\n';
    close(serverFd);
    return;
  }
  // The main server is usually started with sudo for port 80, while the local
  // processor runs as the regular user.
  if (chmod(kProcessorSocketPath, 0666) == -1) {
    std::cerr << "Could not set local processor bridge permissions: "
              << std::strerror(errno) << '\n';
  }
  std::cout << "Waiting for local server_side_process" << std::endl;
  while (true) {
    const int clientFd = accept(serverFd, nullptr, nullptr);
    if (clientFd != -1) std::thread(handleProcessor, clientFd).detach();
  }
}

std::string getClientIp(const sockaddr_in& address) {
  char ip[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) return "unknown";
  return ip;
}

}  // namespace

void runVideoStreamServer() {
  std::thread(runProcessorBridge).detach();
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
    std::cerr << "Video stream server could not listen on port 5001: " << std::strerror(errno) << '\n';
    close(serverFd);
    return;
  }
  std::cout << "Live video stream listening on 0.0.0.0:5001" << std::endl;
  while (true) {
    sockaddr_in clientAddress{};
    socklen_t length = sizeof(clientAddress);
    const int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &length);
    if (clientFd == -1) continue;
    std::array<std::uint8_t, 4> firstBytes{};
    if (!receiveAll(clientFd, firstBytes.data(), firstBytes.size())) {
      close(clientFd);
      continue;
    }
    const std::string clientIp = getClientIp(clientAddress);
    if (std::memcmp(firstBytes.data(), "SUB1", 4) == 0) {
      std::array<char, kSessionIdBytes> sessionBytes{};
      if (!receiveAll(clientFd, sessionBytes.data(), sessionBytes.size())) {
        close(clientFd);
        continue;
      }
      std::thread(handleSubscriber, clientFd,
                  std::string(sessionBytes.data(), sessionBytes.size())).detach();
    } else if (std::memcmp(firstBytes.data(), "STR1", 4) == 0) {
      std::array<char, kSessionIdBytes> sessionBytes{};
      if (!receiveAll(clientFd, sessionBytes.data(), sessionBytes.size()) ||
          !receiveAll(clientFd, firstBytes.data(), firstBytes.size())) {
        close(clientFd);
        continue;
      }
      std::cout << "Live video started: " << clientIp << std::endl;
      std::thread(handleVideoClient, clientFd, clientIp,
                  std::string(sessionBytes.data(), sessionBytes.size()), firstBytes).detach();
    } else {
      close(clientFd);
    }
  }
}
