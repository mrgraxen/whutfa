#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static void parse_simple_yaml(const std::string& content, HandlerConfig& cfg) {
  std::string section;
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line.back() == ':') {
      section = line.substr(0, line.size() - 1);
      continue;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto key = trim(line.substr(0, colon));
    auto val = trim(line.substr(colon + 1));
    if (!val.empty() && val.front() == '"') val = val.substr(1, val.size() - 2);

    if (section == "head_unit") {
      if (key == "manufacturer") cfg.manufacturer = val;
      if (key == "model") cfg.model = val;
      if (key == "name") cfg.name = val;
    } else if (section == "video") {
      if (key == "width") cfg.video_width = static_cast<uint32_t>(std::stoul(val));
      if (key == "height") cfg.video_height = static_cast<uint32_t>(std::stoul(val));
      if (key == "fps") cfg.video_fps = static_cast<uint32_t>(std::stoul(val));
    } else if (section == "ipc") {
      if (key == "bridge_socket") cfg.bridge_socket = val;
      if (key == "video_socket") cfg.video_socket = val;
      if (key == "audio_media_socket") cfg.audio_media_socket = val;
      if (key == "audio_speech_socket") cfg.audio_speech_socket = val;
    } else if (section == "sensors" && key == "gps") {
      /* nested skipped */
    } else if (section == "gps" || (section == "sensors" && key == "enabled")) {
      if (key == "enabled" && val == "true") cfg.gps_enabled = true;
      if (key == "latitude") cfg.gps_lat = std::stod(val);
      if (key == "longitude") cfg.gps_lon = std::stod(val);
    }
  }
}

HandlerConfig load_config(const std::string& path) {
  HandlerConfig cfg;
  if (const char* stub = std::getenv("AA_STUB")) {
    if (std::string(stub) == "1") cfg.stub_mode = true;
  }
  std::ifstream f(path);
  if (f) {
    std::stringstream buf;
    buf << f.rdbuf();
    parse_simple_yaml(buf.str(), cfg);
  }
  return cfg;
}
