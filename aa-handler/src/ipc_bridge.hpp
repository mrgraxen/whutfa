// WHUTFA — Web Head Unit Transformator For Android
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/**
 * IpcBridge owns four Unix listening sockets and pipes data between the C++
 * Android Auto handler and the Node.js server.
 *
 *   bridge_socket        : JSON line-delimited control messages
 *                          - C++ -> Node: session state, video_config
 *                          - Node -> C++: touch events (consumed via TouchCallback)
 *   video_socket         : length-prefixed H.264 frames
 *                          [uint32_be payload_len][uint64_be timestamp_us][h264 bytes]
 *   audio_media_socket   : audio packets
 *                          [u8 type][u32_be rate][u8 ch][u16_be bits][u64_be ts][u32_be len][pcm]
 *   audio_speech_socket  : same framing as media audio
 *
 * Single-client policy per socket. When a new client connects on a socket the
 * previous connection is closed.
 */
class IpcBridge {
 public:
  using TouchCallback = std::function<void(double x_norm, double y_norm, int action)>;

  IpcBridge(const std::string& bridge, const std::string& video,
            const std::string& audio_media, const std::string& audio_speech);
  ~IpcBridge();

  bool start();
  void stop();

  // Outbound to server (non-blocking, drops on backpressure)
  void emit_control(const std::string& json_line);
  void queue_video(uint64_t timestamp_us, const uint8_t* data, size_t len);
  void queue_audio(uint8_t type, uint32_t sample_rate, uint8_t channels, uint16_t bits,
                   uint64_t timestamp_us, const uint8_t* pcm, size_t len);

  void set_touch_callback(TouchCallback cb);

  std::atomic<uint64_t> dropped_video{0};
  std::atomic<uint64_t> dropped_audio{0};

 private:
  struct Socket {
    std::string path;
    int listen_fd{-1};
    int client_fd{-1};
    std::mutex mu;
    std::condition_variable cv;
    std::queue<std::vector<uint8_t>> tx_queue;
    std::thread accept_thread;
    std::thread send_thread;
  };

  std::string bridge_path_;
  std::string video_path_;
  std::string audio_media_path_;
  std::string audio_speech_path_;

  Socket bridge_sock_;
  Socket video_sock_;
  Socket audio_media_sock_;
  Socket audio_speech_sock_;

  std::thread bridge_read_thread_;
  std::atomic<bool> running_{false};
  TouchCallback touch_cb_;
  std::mutex touch_cb_mu_;

  static constexpr size_t kMaxVideoQueue = 5;
  static constexpr size_t kMaxAudioQueue = 32;

  bool open_listener(Socket& s);
  void start_socket_workers(Socket& s, size_t max_queue);
  void accept_loop(Socket& s);
  void send_loop(Socket& s);
  void bridge_read_loop();
  void enqueue(Socket& s, std::vector<uint8_t>&& packet, size_t max_queue,
               std::atomic<uint64_t>& dropped_counter);
};
