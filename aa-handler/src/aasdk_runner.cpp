// WHUTFA — Web Head Unit Transformator For Android
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real Android Auto USB runner. Spins up libusb + asio + aasdk USBHub and
// waits for an Android phone to attach. When AOAP setup succeeds it hands the
// resulting AOAPDevice to a whutfa::aa::Session which drives the full
// projection protocol.
//
// Shape modelled after openauto's autoapp/App.cpp but stripped of Qt and any
// GUI projection sinks — frames go into the IpcBridge instead.

#include "config.hpp"
#include "ipc_bridge.hpp"
#include "stub_handler.hpp"
#include "aa/Session.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <libusb.h>

#include <aasdk/USB/AccessoryModeQueryChain.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/AOAPDevice.hpp>
#include <aasdk/USB/ConnectedAccessoriesEnumerator.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/Error/Error.hpp>
#include <aasdk/Error/ErrorCode.hpp>

namespace {

// Global handle to the active session so a signal handler can tear it down
// cleanly. We deliberately keep this minimal — the runner already manages
// lifetime explicitly during normal operation.
std::weak_ptr<whutfa::aa::Session> g_active_session;
std::atomic<bool> g_shutdown{false};
boost::asio::io_service* g_io_ptr = nullptr;

void on_signal(int /*signo*/) {
  g_shutdown.store(true);
  if (auto session = g_active_session.lock()) {
    try {
      session->stop();
    } catch (...) {
    }
  }
  if (g_io_ptr) g_io_ptr->stop();
}

class UsbEventThread {
 public:
  UsbEventThread(libusb_context* ctx, aasdk::usb::IUSBWrapper& wrapper)
      : ctx_(ctx), wrapper_(wrapper) {}

  void start() {
    running_.store(true);
    thread_ = std::thread([this] {
      while (running_.load()) {
        try {
          wrapper_.handleEvents();
        } catch (...) {
        }
      }
    });
  }

  void stop() {
    running_.store(false);
    // Wake the libusb event loop so handleEvents() returns. Calling
    // libusb_interrupt_event_handler() requires libusb >= 1.0.21 which is
    // available on Debian bookworm.
    if (ctx_) libusb_interrupt_event_handler(ctx_);
    if (thread_.joinable()) thread_.join();
  }

 private:
  libusb_context* ctx_;
  aasdk::usb::IUSBWrapper& wrapper_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

class Runner : public std::enable_shared_from_this<Runner> {
 public:
  Runner(boost::asio::io_service& io, aasdk::usb::IUSBWrapper& usb_wrapper,
         aasdk::usb::IAccessoryModeQueryChainFactory& query_chain_factory,
         IpcBridge& ipc, const HandlerConfig& cfg)
      : io_(io),
        usb_wrapper_(usb_wrapper),
        query_chain_factory_(query_chain_factory),
        ipc_(ipc),
        cfg_(cfg) {}

  void start() {
    auto self = shared_from_this();
    // First, check for already-plugged accessories (they may have skipped
    // hotplug if they were attached before we started).
    enumerator_ = std::make_shared<aasdk::usb::ConnectedAccessoriesEnumerator>(
        usb_wrapper_, io_, query_chain_factory_);
    auto enum_promise = aasdk::usb::IConnectedAccessoriesEnumerator::Promise::defer(io_);
    enum_promise->then(
        [self](bool) {
          std::cerr << "[aa-runner] enumeration complete, listening for hotplug\n";
          self->startHub();
        },
        [self](const aasdk::error::Error& err) {
          std::cerr << "[aa-runner] enumeration error: " << err.what() << "\n";
          self->startHub();
        });
    enumerator_->enumerate(std::move(enum_promise));
  }

  void stop() {
    if (hub_) hub_->cancel();
    if (enumerator_) enumerator_->cancel();
  }

 private:
  void startHub() {
    if (g_shutdown.load()) return;
    hub_ = std::make_shared<aasdk::usb::USBHub>(usb_wrapper_, io_, query_chain_factory_);
    auto self = shared_from_this();
    auto hub_promise = aasdk::usb::IUSBHub::Promise::defer(io_);
    hub_promise->then(
        [self](aasdk::usb::DeviceHandle handle) {
          self->onDevice(std::move(handle));
        },
        [self](const aasdk::error::Error& err) {
          if (err.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;
          std::cerr << "[aa-runner] hub error: " << err.what() << "\n";
          // Retry the hub on transient errors after a short delay
          if (!g_shutdown.load()) {
            self->scheduleRestart();
          }
        });
    hub_->start(std::move(hub_promise));
    std::cerr << "[aa-runner] USB hub started, waiting for AOAP device\n";
  }

  void onDevice(aasdk::usb::DeviceHandle handle) {
    std::cerr << "[aa-runner] AOAP device attached, starting session\n";
    try {
      auto aoap = aasdk::usb::AOAPDevice::create(usb_wrapper_, io_, handle);
      auto session = std::make_shared<whutfa::aa::Session>(io_, ipc_, cfg_);
      g_active_session = session;
      session->start(std::move(aoap));
      current_session_ = session;
    } catch (const aasdk::error::Error& e) {
      std::cerr << "[aa-runner] failed to create AOAPDevice: " << e.what() << "\n";
    } catch (const std::exception& e) {
      std::cerr << "[aa-runner] device error: " << e.what() << "\n";
    }
    // After a session ends the user can plug the phone again. Start hub again
    // so we can pick up another connection.
    auto self = shared_from_this();
    if (!g_shutdown.load()) {
      self->startHub();
    }
  }

  void scheduleRestart() {
    auto timer = std::make_shared<boost::asio::deadline_timer>(io_);
    timer->expires_from_now(boost::posix_time::seconds(2));
    auto self = shared_from_this();
    timer->async_wait([self, timer](const boost::system::error_code&) {
      if (!g_shutdown.load()) self->startHub();
    });
  }

  boost::asio::io_service& io_;
  aasdk::usb::IUSBWrapper& usb_wrapper_;
  aasdk::usb::IAccessoryModeQueryChainFactory& query_chain_factory_;
  IpcBridge& ipc_;
  HandlerConfig cfg_;

  std::shared_ptr<aasdk::usb::IConnectedAccessoriesEnumerator> enumerator_;
  std::shared_ptr<aasdk::usb::IUSBHub> hub_;
  std::shared_ptr<whutfa::aa::Session> current_session_;
};

}  // namespace

int run_aasdk_handler(const HandlerConfig& cfg) {
  std::cerr << "[aa-runner] starting real Android Auto handler\n";

  // IPC bridge to Node server.
  IpcBridge ipc(cfg.bridge_socket, cfg.video_socket, cfg.audio_media_socket,
                cfg.audio_speech_socket);
  if (!ipc.start()) {
    std::cerr << "[aa-runner] failed to start IPC bridge\n";
    return 1;
  }

  // libusb context.
  libusb_context* ctx = nullptr;
  if (int r = libusb_init(&ctx); r != 0) {
    std::cerr << "[aa-runner] libusb_init failed: " << libusb_error_name(r) << "\n";
    return 1;
  }
  if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
    std::cerr << "[aa-runner] WARNING: libusb hotplug not supported on this host\n";
  }

  boost::asio::io_service io_service;
  g_io_ptr = &io_service;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  aasdk::usb::USBWrapper usb_wrapper(ctx);
  aasdk::usb::AccessoryModeQueryFactory query_factory(usb_wrapper, io_service);
  aasdk::usb::AccessoryModeQueryChainFactory query_chain_factory(usb_wrapper, io_service,
                                                                 query_factory);

  UsbEventThread usb_events(ctx, usb_wrapper);
  usb_events.start();

  auto runner = std::make_shared<Runner>(io_service, usb_wrapper, query_chain_factory,
                                         ipc, cfg);
  runner->start();

  // Run the asio loop on the main thread. aasdk dispatches USB transfer
  // callbacks via the usb_events thread which post into io_service strands,
  // so a single io_service run thread is enough.
  try {
    boost::asio::io_service::work keep_alive(io_service);
    io_service.run();
  } catch (const std::exception& e) {
    std::cerr << "[aa-runner] io_service exception: " << e.what() << "\n";
  }

  runner->stop();
  usb_events.stop();
  ipc.stop();
  libusb_exit(ctx);
  g_io_ptr = nullptr;
  std::cerr << "[aa-runner] shutdown complete\n";
  return 0;
}
