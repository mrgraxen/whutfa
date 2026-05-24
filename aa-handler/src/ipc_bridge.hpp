#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class IpcBridge {
 public:
  explicit IpcBridge(const std::string& bridge, const std::string& video,
                     const std::string& audio_media, const std::string& audio_speech);
  ~IpcBridge();

  bool start();
  void stop();

  void emit_control(const std::string& json_line);
  void queue_video(uint64_t timestamp_us, std::vector<uint8_t> data);
  void queue_audio(uint8_t type, uint32_t sample_rate, uint8_t channels, uint16_t bits,
                   uint64_t timestamp_us, std::vector<uint8_t> pcm);

  std::atomic<uint64_t> dropped_frames{0};

 private:
  std::string bridge_path_;
  std::string video_path_;
  std::string audio_media_path_;
  std::string audio_speech_path_;

  int bridge_fd_{-1};
  int video_fd_{-1};
  int audio_media_fd_{-1};
  int bridge_client_fd_{-1};

  std::thread video_thread_;
  std::thread bridge_accept_thread_;
  std::atomic<bool> running_{false};

  std::mutex bridge_mu_;
  std::mutex video_mu_;
  std::queue<std::pair<uint64_t, std::vector<uint8_t>>> video_q_;
  static constexpr size_t kMaxVideoQueue = 3;

  bool listen_unix(const std::string& path, int& out_fd);
  void video_writer_loop();
  void write_frame(int fd, const std::vector<uint8_t>& packet);
};
