#include "stub_handler.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {

std::vector<uint8_t> read_fixture(const std::string& name) {
  std::ifstream f("/app/scripts/fixtures/" + name, std::ios::binary);
  if (!f) f.open("scripts/fixtures/" + name, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string json_session(const char* state) {
  return std::string("{\"event\":\"session\",\"state\":\"") + state + "\"}";
}

}  // namespace

int run_stub_handler(const HandlerConfig& cfg) {
  IpcBridge ipc(cfg.bridge_socket, cfg.video_socket, cfg.audio_media_socket, cfg.audio_speech_socket);
  if (!ipc.start()) {
    std::cerr << "[aa-handler] failed to start IPC listeners\n";
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ipc.emit_control(json_session("connecting"));

  std::ostringstream vc;
  vc << "{\"event\":\"video_config\",\"width\":" << cfg.video_width << ",\"height\":" << cfg.video_height
     << ",\"fps\":" << cfg.video_fps << "}";
  ipc.emit_control(vc.str());
  ipc.emit_control(json_session("active"));

  auto h264 = read_fixture("sample.h264");
  auto pcm = read_fixture("sample-media.pcm");
  if (h264.empty()) {
    std::cerr << "[aa-handler] warning: no sample.h264 fixture\n";
  }

  uint64_t ts = 0;
  while (true) {
    if (!h264.empty()) {
      ipc.queue_video(ts, h264);
      ts += 33'000;
    }
    if (!pcm.empty()) {
      std::vector<uint8_t> chunk(pcm.begin(), pcm.begin() + std::min(pcm.size(), size_t{1920}));
      ipc.queue_audio(1, cfg.media_sample_rate, 2, 16, ts, chunk);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  return 0;
}
