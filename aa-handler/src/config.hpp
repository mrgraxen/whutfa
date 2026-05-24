#pragma once

#include <cstdint>
#include <string>

struct HandlerConfig {
  std::string manufacturer = "PiHeadUnit";
  std::string model = "WebHU-1";
  std::string name = "Raspberry Pi Head Unit";
  uint32_t video_width = 1280;
  uint32_t video_height = 720;
  uint32_t video_fps = 30;
  uint32_t video_dpi = 160;
  uint32_t media_sample_rate = 48000;
  uint32_t speech_sample_rate = 16000;
  std::string bridge_socket = "/tmp/aa-bridge.sock";
  std::string video_socket = "/tmp/aa-video.sock";
  std::string audio_media_socket = "/tmp/aa-audio-media.sock";
  std::string audio_speech_socket = "/tmp/aa-audio-speech.sock";
  bool gps_enabled = false;
  double gps_lat = 0;
  double gps_lon = 0;
  bool stub_mode = false;
};

HandlerConfig load_config(const std::string& path);
