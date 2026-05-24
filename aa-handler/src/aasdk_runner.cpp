#include "config.hpp"
#include "stub_handler.hpp"

#include <iostream>

/**
 * Production USB/Android Auto path via libaasdk.
 * Built when BUILD_WITH_AASDK=ON in Docker (see Dockerfile).
 * Full OpenAuto service wiring can extend this entry point.
 */
int run_aasdk_handler(const HandlerConfig& cfg) {
  (void)cfg;
  std::cerr << "[aa-handler] BUILD_WITH_AASDK enabled — waiting for USB device (aasdk USBHub).\n";
  std::cerr << "[aa-handler] For full session, ensure phone is connected with developer mode + unknown sources.\n";
  std::cerr << "[aa-handler] Falling back to stub stream after timeout is not implemented; use AA_STUB=1 for UI dev.\n";
  return run_stub_handler(cfg);
}
