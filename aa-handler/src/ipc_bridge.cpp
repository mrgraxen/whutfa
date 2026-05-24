#include "ipc_bridge.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

bool unlink_path(const std::string& path) {
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

}  // namespace

IpcBridge::IpcBridge(const std::string& bridge, const std::string& video,
                     const std::string& audio_media, const std::string& audio_speech)
    : bridge_path_(bridge),
      video_path_(video),
      audio_media_path_(audio_media),
      audio_speech_path_(audio_speech) {}

IpcBridge::~IpcBridge() { stop(); }

bool IpcBridge::listen_unix(const std::string& path, int& out_fd) {
  unlink_path(path);
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) return false;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return false;
  }
  if (::listen(fd, 4) < 0) {
    ::close(fd);
    return false;
  }
  out_fd = fd;
  return true;
}

bool IpcBridge::start() {
  if (!listen_unix(bridge_path_, bridge_fd_)) return false;
  if (!listen_unix(video_path_, video_fd_)) return false;
  if (!listen_unix(audio_media_path_, audio_media_fd_)) return false;
  running_ = true;
  video_thread_ = std::thread([this] { video_writer_loop(); });
  bridge_accept_thread_ = std::thread([this] {
    while (running_) {
      int client = ::accept(bridge_fd_, nullptr, nullptr);
      if (client < 0) {
        if (errno == EINTR) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      std::lock_guard<std::mutex> lock(bridge_mu_);
      if (bridge_client_fd_ >= 0) ::close(bridge_client_fd_);
      bridge_client_fd_ = client;
    }
  });
  return true;
}

void IpcBridge::stop() {
  running_ = false;
  if (bridge_fd_ >= 0) ::shutdown(bridge_fd_, SHUT_RDWR);
  if (video_fd_ >= 0) ::shutdown(video_fd_, SHUT_RDWR);
  if (audio_media_fd_ >= 0) ::shutdown(audio_media_fd_, SHUT_RDWR);
  if (bridge_fd_ >= 0) ::close(bridge_fd_);
  if (video_fd_ >= 0) ::close(video_fd_);
  if (audio_media_fd_ >= 0) ::close(audio_media_fd_);
  bridge_fd_ = video_fd_ = audio_media_fd_ = -1;
  {
    std::lock_guard<std::mutex> lock(bridge_mu_);
    if (bridge_client_fd_ >= 0) ::close(bridge_client_fd_);
    bridge_client_fd_ = -1;
  }
  if (video_thread_.joinable()) video_thread_.join();
  if (bridge_accept_thread_.joinable()) bridge_accept_thread_.join();
}

void IpcBridge::emit_control(const std::string& json_line) {
  std::lock_guard<std::mutex> lock(bridge_mu_);
  if (bridge_client_fd_ < 0) return;
  std::string msg = json_line + "\n";
  ::write(bridge_client_fd_, msg.data(), msg.size());
}

void IpcBridge::queue_video(uint64_t timestamp_us, std::vector<uint8_t> data) {
  std::lock_guard<std::mutex> lock(video_mu_);
  if (video_q_.size() >= kMaxVideoQueue) {
    video_q_.pop();
    dropped_frames++;
  }
  video_q_.push({timestamp_us, std::move(data)});
}

void IpcBridge::queue_audio(uint8_t type, uint32_t sample_rate, uint8_t channels,
                            uint16_t bits, uint64_t timestamp_us, std::vector<uint8_t> pcm) {
  std::vector<uint8_t> packet(20 + pcm.size());
  packet[0] = type;
  uint32_t sr_be = htonl(sample_rate);
  std::memcpy(&packet[1], &sr_be, 4);
  packet[5] = channels;
  uint16_t bits_be = htons(bits);
  std::memcpy(&packet[6], &bits_be, 2);
  uint64_t ts_be = htobe64(timestamp_us);
  std::memcpy(&packet[8], &ts_be, 8);
  uint32_t len_be = htonl(static_cast<uint32_t>(pcm.size()));
  std::memcpy(&packet[16], &len_be, 4);
  std::memcpy(packet.data() + 20, pcm.data(), pcm.size());

  int client = ::accept(audio_media_fd_, nullptr, nullptr);
  if (client >= 0) {
    ::write(client, packet.data(), packet.size());
    ::close(client);
  }
}

void IpcBridge::video_writer_loop() {
  while (running_) {
    std::pair<uint64_t, std::vector<uint8_t>> item;
    bool have = false;
    {
      std::lock_guard<std::mutex> lock(video_mu_);
      if (!video_q_.empty()) {
        item = std::move(video_q_.front());
        video_q_.pop();
        have = true;
      }
    }
    if (!have) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    const auto& data = item.second;
    std::vector<uint8_t> packet(4 + 8 + data.size());
    uint32_t payload_len = static_cast<uint32_t>(8 + data.size());
    uint32_t len_be = htonl(payload_len);
    std::memcpy(packet.data(), &len_be, 4);
    uint64_t ts_be = htobe64(item.first);
    std::memcpy(packet.data() + 4, &ts_be, 8);
    std::memcpy(packet.data() + 12, data.data(), data.size());

    int client = ::accept(video_fd_, nullptr, nullptr);
    if (client >= 0) {
      ::write(client, packet.data(), packet.size());
      ::close(client);
    }
  }
}
