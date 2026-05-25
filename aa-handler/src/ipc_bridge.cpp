// WHUTFA — Web Head Unit Transformator For Android
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc_bridge.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace {

bool unlink_path(const std::string& path) {
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool write_all(int fd, const uint8_t* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = ::write(fd, data + written, len - written);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

IpcBridge::IpcBridge(const std::string& bridge, const std::string& video,
                     const std::string& audio_media, const std::string& audio_speech)
    : bridge_path_(bridge),
      video_path_(video),
      audio_media_path_(audio_media),
      audio_speech_path_(audio_speech) {
  bridge_sock_.path = bridge_path_;
  video_sock_.path = video_path_;
  audio_media_sock_.path = audio_media_path_;
  audio_speech_sock_.path = audio_speech_path_;
}

IpcBridge::~IpcBridge() { stop(); }

bool IpcBridge::open_listener(Socket& s) {
  unlink_path(s.path);
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (s.path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return false;
  }
  std::strncpy(addr.sun_path, s.path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "[ipc] bind " << s.path << " failed: " << std::strerror(errno) << "\n";
    ::close(fd);
    return false;
  }
  if (::listen(fd, 4) < 0) {
    std::cerr << "[ipc] listen " << s.path << " failed: " << std::strerror(errno) << "\n";
    ::close(fd);
    return false;
  }
  s.listen_fd = fd;
  return true;
}

bool IpcBridge::start() {
  if (!open_listener(bridge_sock_)) return false;
  if (!open_listener(video_sock_)) return false;
  if (!open_listener(audio_media_sock_)) return false;
  if (!open_listener(audio_speech_sock_)) return false;
  running_ = true;
  start_socket_workers(bridge_sock_, 64);
  start_socket_workers(video_sock_, kMaxVideoQueue);
  start_socket_workers(audio_media_sock_, kMaxAudioQueue);
  start_socket_workers(audio_speech_sock_, kMaxAudioQueue);
  bridge_read_thread_ = std::thread([this] { bridge_read_loop(); });
  std::cerr << "[ipc] listening on " << bridge_path_ << ", " << video_path_ << ", "
            << audio_media_path_ << ", " << audio_speech_path_ << "\n";
  return true;
}

void IpcBridge::start_socket_workers(Socket& s, size_t /*max_queue*/) {
  s.accept_thread = std::thread([this, &s] { accept_loop(s); });
  s.send_thread = std::thread([this, &s] { send_loop(s); });
}

void IpcBridge::stop() {
  if (!running_.exchange(false)) return;
  auto shutdown_socket = [](Socket& s) {
    if (s.listen_fd >= 0) {
      ::shutdown(s.listen_fd, SHUT_RDWR);
      ::close(s.listen_fd);
      s.listen_fd = -1;
    }
    {
      std::lock_guard<std::mutex> lock(s.mu);
      if (s.client_fd >= 0) {
        ::shutdown(s.client_fd, SHUT_RDWR);
        ::close(s.client_fd);
        s.client_fd = -1;
      }
      s.cv.notify_all();
    }
    if (s.accept_thread.joinable()) s.accept_thread.join();
    if (s.send_thread.joinable()) s.send_thread.join();
  };
  shutdown_socket(bridge_sock_);
  shutdown_socket(video_sock_);
  shutdown_socket(audio_media_sock_);
  shutdown_socket(audio_speech_sock_);
  if (bridge_read_thread_.joinable()) bridge_read_thread_.join();
}

void IpcBridge::accept_loop(Socket& s) {
  while (running_) {
    int client = ::accept(s.listen_fd, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (!running_) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    // Disable Nagle-like blocking by setting send buffer expectations; SOCK_STREAM Unix sockets
    // generally don't block unless the peer is slow. Keep simple.
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.client_fd >= 0) {
      ::shutdown(s.client_fd, SHUT_RDWR);
      ::close(s.client_fd);
    }
    s.client_fd = client;
    s.cv.notify_all();
    std::cerr << "[ipc] client connected on " << s.path << "\n";
  }
}

void IpcBridge::send_loop(Socket& s) {
  while (running_) {
    std::vector<uint8_t> packet;
    int fd = -1;
    {
      std::unique_lock<std::mutex> lock(s.mu);
      s.cv.wait(lock, [&] { return !running_ || (s.client_fd >= 0 && !s.tx_queue.empty()); });
      if (!running_) return;
      if (s.tx_queue.empty()) continue;
      packet = std::move(s.tx_queue.front());
      s.tx_queue.pop();
      fd = s.client_fd;
    }
    if (fd < 0) continue;
    if (!write_all(fd, packet.data(), packet.size())) {
      std::lock_guard<std::mutex> lock(s.mu);
      if (s.client_fd == fd) {
        ::shutdown(s.client_fd, SHUT_RDWR);
        ::close(s.client_fd);
        s.client_fd = -1;
      }
    }
  }
}

void IpcBridge::enqueue(Socket& s, std::vector<uint8_t>&& packet, size_t max_queue,
                        std::atomic<uint64_t>& dropped_counter) {
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.client_fd < 0) {
    // No subscriber; drop silently to avoid runaway memory.
    return;
  }
  while (s.tx_queue.size() >= max_queue) {
    s.tx_queue.pop();
    dropped_counter++;
  }
  s.tx_queue.push(std::move(packet));
  s.cv.notify_one();
}

void IpcBridge::emit_control(const std::string& json_line) {
  std::vector<uint8_t> msg(json_line.size() + 1);
  std::memcpy(msg.data(), json_line.data(), json_line.size());
  msg.back() = '\n';
  enqueue(bridge_sock_, std::move(msg), 256, dropped_video);  // re-use counter, fine
}

void IpcBridge::queue_video(uint64_t timestamp_us, const uint8_t* data, size_t len) {
  std::vector<uint8_t> packet(4 + 8 + len);
  uint32_t payload_len = static_cast<uint32_t>(8 + len);
  uint32_t len_be = htonl(payload_len);
  std::memcpy(packet.data(), &len_be, 4);
  uint64_t ts_be = htobe64(timestamp_us);
  std::memcpy(packet.data() + 4, &ts_be, 8);
  std::memcpy(packet.data() + 12, data, len);
  enqueue(video_sock_, std::move(packet), kMaxVideoQueue, dropped_video);
}

void IpcBridge::queue_audio(uint8_t type, uint32_t sample_rate, uint8_t channels,
                            uint16_t bits, uint64_t timestamp_us, const uint8_t* pcm,
                            size_t len) {
  std::vector<uint8_t> packet(20 + len);
  packet[0] = type;
  uint32_t sr_be = htonl(sample_rate);
  std::memcpy(&packet[1], &sr_be, 4);
  packet[5] = channels;
  uint16_t bits_be = htons(bits);
  std::memcpy(&packet[6], &bits_be, 2);
  uint64_t ts_be = htobe64(timestamp_us);
  std::memcpy(&packet[8], &ts_be, 8);
  uint32_t len_be = htonl(static_cast<uint32_t>(len));
  std::memcpy(&packet[16], &len_be, 4);
  std::memcpy(packet.data() + 20, pcm, len);
  Socket& target = (type == 2) ? audio_speech_sock_ : audio_media_sock_;
  enqueue(target, std::move(packet), kMaxAudioQueue, dropped_audio);
}

void IpcBridge::set_touch_callback(TouchCallback cb) {
  std::lock_guard<std::mutex> lock(touch_cb_mu_);
  touch_cb_ = std::move(cb);
}

void IpcBridge::bridge_read_loop() {
  std::string line_buf;
  std::vector<char> read_buf(4096);
  while (running_) {
    int fd;
    {
      std::unique_lock<std::mutex> lock(bridge_sock_.mu);
      bridge_sock_.cv.wait(lock, [&] { return !running_ || bridge_sock_.client_fd >= 0; });
      if (!running_) return;
      fd = bridge_sock_.client_fd;
    }
    if (fd < 0) continue;
    while (running_) {
      ssize_t n = ::read(fd, read_buf.data(), read_buf.size());
      if (n <= 0) {
        if (n < 0 && errno == EINTR) continue;
        break;
      }
      line_buf.append(read_buf.data(), static_cast<size_t>(n));
      size_t pos;
      while ((pos = line_buf.find('\n')) != std::string::npos) {
        std::string line = line_buf.substr(0, pos);
        line_buf.erase(0, pos + 1);
        // very small JSON parser for {"type":"touch","action":N,"x":N,"y":N}
        double x = 0, y = 0;
        int action = -1;
        auto find_num = [&line](const char* key, double& out) {
          auto p = line.find(key);
          if (p == std::string::npos) return false;
          p = line.find(':', p);
          if (p == std::string::npos) return false;
          ++p;
          while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
          char* end = nullptr;
          out = std::strtod(line.c_str() + p, &end);
          return end != line.c_str() + p;
        };
        double a;
        if (find_num("\"action\"", a)) action = static_cast<int>(a);
        find_num("\"x\"", x);
        find_num("\"y\"", y);
        if (action >= 0) {
          TouchCallback cb;
          {
            std::lock_guard<std::mutex> lock(touch_cb_mu_);
            cb = touch_cb_;
          }
          if (cb) cb(x, y, action);
        }
      }
    }
  }
}
