#include "http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kWebPort = 80;
constexpr std::size_t kMaxRequestBytes = 30 * 1024 * 1024;

const char* kUploadPage = R"HTML(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Face upload</title></head><body>
<h2>Face database upload</h2>
<form method="post" action="/upload" enctype="multipart/form-data">
  <p><label>First name <input name="firstName" required pattern="[A-Za-z0-9_-]+"></label></p>
  <p><label>UID <input name="uid" required pattern="[A-Za-z0-9_-]+"></label></p>
  <p><label>Photo 1 <input name="photo1" type="file" accept="image/*" required></label></p>
  <p><label>Photo 2 <input name="photo2" type="file" accept="image/*" required></label></p>
  <p><label>Photo 3 <input name="photo3" type="file" accept="image/*" required></label></p>
  <button type="submit">Upload</button>
</form></body></html>)HTML";

struct UploadedFile {
  std::string filename;
  std::string contentType;
  std::string bytes;
};

struct Upload {
  std::string firstName;
  std::string uid;
  std::map<std::string, UploadedFile> photos;
};

std::string toLower(std::string value) {
  for (char& character : value) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

bool isSafeIdentifier(const std::string& value) {
  if (value.empty() || value.size() > 64) return false;
  for (unsigned char character : value) {
    if (!std::isalnum(character) && character != '_' && character != '-') return false;
  }
  return true;
}

std::optional<std::string> headerValue(const std::string& headers, const std::string& name) {
  const std::string lowerHeaders = toLower(headers);
  const std::string prefix = toLower(name) + ":";
  const std::size_t start = lowerHeaders.find(prefix);
  if (start == std::string::npos) return std::nullopt;
  const std::size_t valueStart = start + prefix.size();
  const std::size_t end = headers.find("\r\n", valueStart);
  std::string value = headers.substr(valueStart, end - valueStart);
  const std::size_t first = value.find_first_not_of(" \t");
  return first == std::string::npos ? "" : value.substr(first);
}

std::optional<std::string> dispositionValue(const std::string& headers, const std::string& key) {
  const std::string needle = key + "=\"";
  const std::size_t start = headers.find(needle);
  if (start == std::string::npos) return std::nullopt;
  const std::size_t valueStart = start + needle.size();
  const std::size_t end = headers.find('"', valueStart);
  if (end == std::string::npos) return std::nullopt;
  return headers.substr(valueStart, end - valueStart);
}

std::optional<Upload> parseUpload(const std::string& contentType, const std::string& body) {
  const std::size_t boundaryStart = contentType.find("boundary=");
  if (boundaryStart == std::string::npos) return std::nullopt;
  std::string boundary = contentType.substr(boundaryStart + 9);
  if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"') {
    boundary = boundary.substr(1, boundary.size() - 2);
  }
  const std::string delimiter = "--" + boundary;
  Upload upload;
  std::size_t position = 0;

  while (true) {
    const std::size_t partStart = body.find(delimiter, position);
    if (partStart == std::string::npos) break;
    position = partStart + delimiter.size();
    if (body.compare(position, 2, "--") == 0) break;
    if (body.compare(position, 2, "\r\n") != 0) return std::nullopt;
    position += 2;

    const std::size_t headersEnd = body.find("\r\n\r\n", position);
    if (headersEnd == std::string::npos) return std::nullopt;
    const std::string partHeaders = body.substr(position, headersEnd - position);
    const std::size_t dataStart = headersEnd + 4;
    const std::size_t dataEnd = body.find("\r\n" + delimiter, dataStart);
    if (dataEnd == std::string::npos) return std::nullopt;
    position = dataEnd + 2;

    const auto fieldName = dispositionValue(partHeaders, "name");
    if (!fieldName) return std::nullopt;
    const std::string data = body.substr(dataStart, dataEnd - dataStart);
    const auto filename = dispositionValue(partHeaders, "filename");
    if (!filename) {
      if (*fieldName == "firstName") upload.firstName = data;
      if (*fieldName == "uid") upload.uid = data;
      continue;
    }
    if (*fieldName != "photo1" && *fieldName != "photo2" && *fieldName != "photo3") {
      return std::nullopt;
    }
    upload.photos[*fieldName] = {*filename, headerValue(partHeaders, "Content-Type").value_or(""), data};
  }

  if (!isSafeIdentifier(upload.firstName) || !isSafeIdentifier(upload.uid) ||
      upload.photos.size() != 3 || !upload.photos.count("photo1") ||
      !upload.photos.count("photo2") || !upload.photos.count("photo3")) {
    return std::nullopt;
  }
  return upload;
}

std::optional<std::string> imageExtension(const UploadedFile& file) {
  if (file.contentType.rfind("image/", 0) != 0) return std::nullopt;
  std::string extension = toLower(fs::path(file.filename).extension().string());
  if (extension.empty() || extension.size() > 8) return std::nullopt;
  for (unsigned char character : extension.substr(1)) {
    if (!std::isalnum(character)) return std::nullopt;
  }
  return extension;
}

bool saveUpload(const Upload& upload) {
  const fs::path directory = fs::path("face_database") /
                             (upload.firstName + "." + upload.uid);
  fs::create_directories(directory);
  for (int index = 1; index <= 3; ++index) {
    const UploadedFile& file = upload.photos.at("photo" + std::to_string(index));
    const auto extension = imageExtension(file);
    if (!extension) return false;
    std::ofstream output(directory / (std::to_string(index) + *extension), std::ios::binary);
    if (!output) return false;
    output.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
    if (!output) return false;
  }
  return true;
}

void sendResponse(int clientFd, int status, const std::string& body) {
  const std::string reason = status == 200 ? "OK" : "Bad Request";
  const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                               "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  send(clientFd, response.data(), response.size(), 0);
}

void handleClient(int clientFd) {
  std::string request;
  char buffer[8192];
  std::size_t expectedBytes = 0;
  while (request.size() < kMaxRequestBytes) {
    const ssize_t received = recv(clientFd, buffer, sizeof(buffer), 0);
    if (received <= 0) return;
    request.append(buffer, static_cast<std::size_t>(received));
    const std::size_t headersEnd = request.find("\r\n\r\n");
    if (headersEnd == std::string::npos) continue;
    const auto contentLength = headerValue(request.substr(0, headersEnd), "Content-Length");
    if (contentLength) {
      try {
        expectedBytes = static_cast<std::size_t>(std::stoull(*contentLength));
      } catch (...) {
        sendResponse(clientFd, 400, "Invalid Content-Length");
        return;
      }
    }
    if (expectedBytes > kMaxRequestBytes || request.size() >= headersEnd + 4 + expectedBytes) break;
  }

  const std::size_t headersEnd = request.find("\r\n\r\n");
  if (headersEnd == std::string::npos) return;
  const std::string headers = request.substr(0, headersEnd);
  const bool isGet = headers.rfind("GET / ", 0) == 0 || headers.rfind("GET /HTTP", 0) == 0;
  if (isGet) {
    sendResponse(clientFd, 200, kUploadPage);
    return;
  }
  if (headers.rfind("POST /upload ", 0) != 0) {
    sendResponse(clientFd, 400, "Unsupported request");
    return;
  }
  const auto contentType = headerValue(headers, "Content-Type");
  const auto upload = contentType ? parseUpload(*contentType, request.substr(headersEnd + 4)) : std::nullopt;
  if (!upload || !saveUpload(*upload)) {
    sendResponse(clientFd, 400, "Upload failed. Provide a name, UID, and three image files.");
    return;
  }
  sendResponse(clientFd, 200, "<h2>Upload saved.</h2>");
}

}  // namespace

void runHttpServer() {
  const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd == -1) {
    std::cerr << "Web server socket failed: " << std::strerror(errno) << '\n';
    return;
  }
  int reuseAddress = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(kWebPort);
  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
      listen(serverFd, 10) == -1) {
    std::cerr << "Web server could not listen on port 80: " << std::strerror(errno) << '\n';
    close(serverFd);
    return;
  }

  std::cout << "Upload page: http://<server-ip>/" << std::endl;
  while (true) {
    const int clientFd = accept(serverFd, nullptr, nullptr);
    if (clientFd == -1) continue;
    handleClient(clientFd);
    close(clientFd);
  }
}
