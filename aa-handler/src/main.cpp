#include "config.hpp"
#include "stub_handler.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef BUILD_WITH_AASDK
int run_aasdk_handler(const HandlerConfig& cfg);
#endif

static volatile std::sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

int main(int argc, char** argv) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::string config_path = "/config/config.yaml";
  if (argc > 1) config_path = argv[1];
  if (const char* p = std::getenv("CONFIG_PATH")) config_path = p;

  HandlerConfig cfg = load_config(config_path);
  std::cout << "[aa-handler] starting stub=" << (cfg.stub_mode ? "yes" : "no") << "\n";

#ifdef BUILD_WITH_AASDK
  if (!cfg.stub_mode) {
    return run_aasdk_handler(cfg);
  }
#endif

  return run_stub_handler(cfg);
}
