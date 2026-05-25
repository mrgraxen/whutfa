// WHUTFA — Web Head Unit Transformator For Android
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "config.hpp"
#include "ipc_bridge.hpp"

namespace aasdk {
namespace usb {
class IAOAPDevice;
}
}  // namespace aasdk

namespace whutfa::aa {

/**
 * Session owns the full Android Auto protocol lifecycle once an AOAP USB
 * device has been opened: transport, cryptor, messenger, control channel and
 * per-service channels. Frames received from the phone are forwarded into the
 * provided IpcBridge, and touch input from the bridge is sent back through
 * the InputSource channel.
 *
 * One Session corresponds to exactly one phone connection. After
 * onQuit() (peer disconnect or fatal error) the owning runner should destroy
 * this instance and create a new one when the next device is attached.
 *
 * Implementation lives entirely inside Session.cpp; this header intentionally
 * stays free of aasdk includes to keep the public surface compact.
 */
class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(boost::asio::io_service& ioService, IpcBridge& ipc, const HandlerConfig& cfg);
  ~Session();

  /** Hand a freshly enumerated AOAP device to the session and start the protocol. */
  void start(std::shared_ptr<aasdk::usb::IAOAPDevice> aoapDevice);

  /** Synchronous teardown; safe to call from signal handler context. */
  void stop();

  /** True while the session is processing the protocol (transport open). */
  bool active() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace whutfa::aa
