#include "http_server.h"
#include "video_stream_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kPort = 5000;
constexpr int kBacklog = 10;
constexpr std::size_t kBufferSize = 1024;

bool sendLine(int fd, const std::string& message) {
  const std::string data = message + "\n";
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t result = send(fd, data.data() + sent, data.size() - sent, 0);
    if (result <= 0) return false;
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

bool parseTerminalMessage(const std::string& line, std::string& ip,
                          std::string& message) {
  if (line.empty() || line.front() != '[') return false;
  const std::size_t closeBracket = line.find(']');
  if (closeBracket == std::string::npos || closeBracket + 2 > line.size() ||
      line[closeBracket + 1] != ' ') {
    return false;
  }
  ip = line.substr(1, closeBracket - 1);
  message = line.substr(closeBracket + 2);
  return !ip.empty() && !message.empty();
}

std::string getClientIp(const sockaddr_in& address) {
  char ip[INET_ADDRSTRLEN]{};
  if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) {
    return "unknown";
  }
  return ip;
}

void closeClient(const std::string& ip, std::unordered_map<std::string, int>& clients) {
  const auto found = clients.find(ip);
  if (found == clients.end()) return;
  close(found->second);
  clients.erase(found);
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  std::thread(runHttpServer).detach();
  std::thread(runVideoStreamServer).detach();

  const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd == -1) {
    std::cerr << "socket failed: " << std::strerror(errno) << '\n';
    return EXIT_FAILURE;
  }

  int reuseAddress = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(kPort);
  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
      listen(serverFd, kBacklog) == -1) {
    std::cerr << "Unable to listen on port " << kPort << ": " << std::strerror(errno)
              << '\n';
    close(serverFd);
    return EXIT_FAILURE;
  }

  std::unordered_map<std::string, int> clients;
  std::unordered_map<int, std::string> clientIps;
  std::unordered_map<int, std::string> pendingMessages;

  std::cout << "Listening on 0.0.0.0:" << kPort << "..." << std::endl;
  std::cout << "Send to a connected phone with: [phone-ip] your message" << std::endl;

  while (true) {
    std::vector<pollfd> fds{{serverFd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    for (const auto& [ip, fd] : clients) fds.push_back({fd, POLLIN, 0});

    if (poll(fds.data(), fds.size(), -1) == -1) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "poll failed: " << std::strerror(errno) << '\n';
      break;
    }

    if (fds[0].revents & POLLIN) {
      sockaddr_in clientAddress{};
      socklen_t length = sizeof(clientAddress);
      const int clientFd =
          accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &length);
      if (clientFd != -1) {
        const std::string ip = getClientIp(clientAddress);
        closeClient(ip, clients);  // Keep the newest connection from an IP.
        clients[ip] = clientFd;
        clientIps[clientFd] = ip;
        pendingMessages[clientFd] = "";
        std::cout << "Phone connected: " << ip << std::endl;
      }
    }

    if (fds[1].revents & POLLIN) {
      std::string line;
      if (std::getline(std::cin, line)) {
        std::string ip;
        std::string message;
        if (parseTerminalMessage(line, ip, message)) {
          const auto found = clients.find(ip);
          if (found != clients.end() && !sendLine(found->second, message)) {
            closeClient(ip, clients);
          }
        } else {
          std::cout << "Use: [phone-ip] your message" << std::endl;
        }
      }
    }

    // Start at 2: index 0 is the listening socket; index 1 is the terminal.
    for (std::size_t index = 2; index < fds.size(); ++index) {
      if (!(fds[index].revents & (POLLIN | POLLHUP | POLLERR))) continue;

      const int clientFd = fds[index].fd;
      const auto ipFound = clientIps.find(clientFd);
      if (ipFound == clientIps.end()) continue;
      const std::string ip = ipFound->second;
      char buffer[kBufferSize];
      const ssize_t received = recv(clientFd, buffer, sizeof(buffer), 0);

      if (received <= 0) {
        closeClient(ip, clients);
        clientIps.erase(clientFd);
        pendingMessages.erase(clientFd);
        continue;
      }

      std::string& pending = pendingMessages[clientFd];
      pending.append(buffer, static_cast<std::size_t>(received));
      std::size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string message = pending.substr(0, newline);
        if (!message.empty() && message.back() == '\r') message.pop_back();
        if (!message.empty()) std::cout << '[' << ip << "] " << message << std::endl;
        pending.erase(0, newline + 1);
      }
    }
  }

  for (const auto& [ip, fd] : clients) close(fd);
  close(serverFd);
}
