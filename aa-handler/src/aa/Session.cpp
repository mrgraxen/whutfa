// WHUTFA — Web Head Unit Transformator For Android
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless Android Auto session orchestrator. Builds on opencardev/aasdk
// (newdev) and pumps every projection frame into the WHUTFA IpcBridge instead
// of into a Qt projection sink the way openauto does.
//
// Layout:
//   - Session::Impl owns the asio strands and channel pointers (channels need a
//     strand reference that outlives them).
//   - VideoSink / AudioSink / InputSource / SensorSource / ControlHandler each
//     hold a reference to "their" strand + channel and implement aasdk's event
//     handler interface for that channel.
//   - Pinger is the same ~5s keepalive timer pattern used by openauto.
//
// Hot paths (per H.264 NAL / PCM packet) keep verbose logging off; aasdk
// already produces info-level trace logs.
#include "Session.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

#include <boost/asio.hpp>

#include <aasdk/Channel/Channel.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp>
#include <aasdk/Channel/InputSource/InputSourceService.hpp>
#include <aasdk/Channel/MediaSink/Audio/AudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/MediaAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/SystemAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Video/Channel/VideoChannel.hpp>
#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/Promise.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Common/Data.hpp>
#include <aasdk/Error/Error.hpp>
#include <aasdk/Error/ErrorCode.hpp>
#include <aasdk/IO/Promise.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Messenger/Timestamp.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/USBTransport.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>

#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusRequest.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusRequestType.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusStateType.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/BatteryStatusNotification.pb.h>
#include <aap_protobuf/service/control/message/ByeByeRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/ConnectionConfiguration.pb.h>
#include <aap_protobuf/service/control/message/DriverPosition.pb.h>
#include <aap_protobuf/service/control/message/HeadUnitInfo.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusType.pb.h>
#include <aap_protobuf/service/control/message/PingConfiguration.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/VoiceSessionNotification.pb.h>
#include <aap_protobuf/service/inputsource/InputSourceService.pb.h>
#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/PointerAction.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>
#include <aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/media/shared/message/Start.pb.h>
#include <aap_protobuf/service/media/shared/message/Stop.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h>
#include <aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusMode.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h>
#include <aap_protobuf/service/sensorsource/SensorSourceService.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h>
#include <aap_protobuf/service/sensorsource/message/NightModeData.pb.h>
#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorRequest.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

namespace whutfa::aa {

namespace pb_ctrl = aap_protobuf::service::control::message;
namespace pb_ms = aap_protobuf::service::media::shared::message;
namespace pb_msink = aap_protobuf::service::media::sink::message;
namespace pb_msrc = aap_protobuf::service::media::source::message;
namespace pb_mvid = aap_protobuf::service::media::video::message;
namespace pb_input = aap_protobuf::service::inputsource::message;
namespace pb_sensor = aap_protobuf::service::sensorsource::message;
namespace pb_shared = aap_protobuf::shared;

namespace {

constexpr int kPingIntervalMs = 5000;

void log_err(const char* tag, const std::string& msg) {
  std::cerr << "[" << tag << "] " << msg << "\n";
}
void log_info(const char* tag, const std::string& msg) {
  std::cerr << "[" << tag << "] " << msg << "\n";
}

uint64_t now_micros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::high_resolution_clock::now().time_since_epoch())
      .count();
}

pb_msink::VideoCodecResolutionType resolution_for(uint32_t w) {
  if (w >= 1920) return pb_msink::VIDEO_1920x1080;
  if (w >= 1280) return pb_msink::VIDEO_1280x720;
  return pb_msink::VIDEO_800x480;
}

pb_msink::VideoFrameRateType fps_for(uint32_t fps) {
  return fps >= 60 ? pb_msink::VIDEO_FPS_60 : pb_msink::VIDEO_FPS_30;
}

// ---------------------------------------------------------------------------
// VideoSink: forwards H.264 NALs into IpcBridge::queue_video.
// ---------------------------------------------------------------------------
class VideoSink final
    : public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler,
      public std::enable_shared_from_this<VideoSink> {
 public:
  using Channel = aasdk::channel::mediasink::video::channel::VideoChannel;

  VideoSink(boost::asio::io_service::strand& strand,
            std::shared_ptr<Channel> channel, IpcBridge& ipc,
            const HandlerConfig& cfg)
      : strand_(strand), channel_(std::move(channel)), ipc_(ipc), cfg_(cfg) {}

  void start() {
    auto self = shared_from_this();
    strand_.dispatch([self] { self->channel_->receive(self); });
  }

  void fillFeatures(pb_ctrl::ServiceDiscoveryResponse& response) const {
    auto* svc = response.add_channels();
    svc->set_id(static_cast<int32_t>(channel_->getId()));
    auto* media = svc->mutable_media_sink_service();
    media->set_available_type(pb_ms::MediaCodecType::MEDIA_CODEC_VIDEO_H264_BP);
    media->set_available_while_in_call(true);
    auto* vc = media->add_video_configs();
    vc->set_codec_resolution(resolution_for(cfg_.video_width));
    vc->set_frame_rate(fps_for(cfg_.video_fps));
    vc->set_density(cfg_.video_dpi);
  }

  void onChannelOpenRequest(const pb_ctrl::ChannelOpenRequest& /*request*/) override {
    log_info("VideoSink", "channel_open");
    pb_ctrl::ChannelOpenResponse response;
    response.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onMediaChannelSetupRequest(const pb_ms::Setup& /*request*/) override {
    log_info("VideoSink", "setup_request");
    pb_ms::Config response;
    response.set_status(pb_ms::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([self] { self->sendFocus(); },
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelSetupResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onMediaChannelStartIndication(const pb_ms::Start& indication) override {
    session_ = indication.session_id();
    log_info("VideoSink", "start session=" + std::to_string(session_));
    channel_->receive(shared_from_this());
  }

  void onMediaChannelStopIndication(const pb_ms::Stop& /*indication*/) override {
    log_info("VideoSink", "stop");
    channel_->receive(shared_from_this());
  }

  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer& buffer) override {
    ipc_.queue_video(static_cast<uint64_t>(timestamp), buffer.cdata, buffer.size);
    pb_msrc::Ack ack;
    ack.set_session_id(session_);
    ack.set_ack(1);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendMediaAckIndication(ack, std::move(promise));
    channel_->receive(self);
  }

  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override {
    onMediaWithTimestampIndication(0, buffer);
  }

  void onVideoFocusRequest(const pb_mvid::VideoFocusRequestNotification& /*request*/) override {
    log_info("VideoSink", "video_focus_request");
    sendFocus();
    channel_->receive(shared_from_this());
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (e.getCode() != aasdk::error::ErrorCode::OPERATION_ABORTED) {
      log_err("VideoSink", std::string("channel error: ") + e.what());
    }
  }

 private:
  void sendFocus() {
    pb_mvid::VideoFocusNotification ind;
    ind.set_focus(pb_mvid::VideoFocusMode::VIDEO_FOCUS_PROJECTED);
    ind.set_unsolicited(false);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendVideoFocusIndication(ind, std::move(promise));
  }

  boost::asio::io_service::strand& strand_;
  std::shared_ptr<Channel> channel_;
  IpcBridge& ipc_;
  HandlerConfig cfg_;
  int32_t session_{-1};
};

// ---------------------------------------------------------------------------
// AudioSink: parameterised over channel kind + IPC stream type. One instance
// per audio service (media/system). Operates on the IAudioMediaSinkService
// base interface so it doesn't have to know which concrete channel class it
// wraps.
// ---------------------------------------------------------------------------
class AudioSink final
    : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler,
      public std::enable_shared_from_this<AudioSink> {
 public:
  using ChannelBase = aasdk::channel::mediasink::audio::AudioMediaSinkService;

  AudioSink(boost::asio::io_service::strand& strand,
            std::shared_ptr<ChannelBase> channel,
            pb_msink::AudioStreamType audio_type, uint32_t sample_rate,
            uint32_t channels, uint32_t bits, uint8_t ipc_type, IpcBridge& ipc)
      : strand_(strand),
        channel_(std::move(channel)),
        audio_type_(audio_type),
        sample_rate_(sample_rate),
        channels_(channels),
        bits_(bits),
        ipc_type_(ipc_type),
        ipc_(ipc) {}

  void start() {
    auto self = shared_from_this();
    strand_.dispatch([self] { self->channel_->receive(self); });
  }

  void fillFeatures(pb_ctrl::ServiceDiscoveryResponse& response) const {
    auto* svc = response.add_channels();
    svc->set_id(static_cast<int32_t>(channel_->getId()));
    auto* media = svc->mutable_media_sink_service();
    media->set_available_type(pb_ms::MediaCodecType::MEDIA_CODEC_AUDIO_PCM);
    media->set_audio_type(audio_type_);
    media->set_available_while_in_call(true);
    auto* cfg = media->add_audio_configs();
    cfg->set_sampling_rate(sample_rate_);
    cfg->set_number_of_bits(bits_);
    cfg->set_number_of_channels(channels_);
  }

  void onChannelOpenRequest(const pb_ctrl::ChannelOpenRequest& /*request*/) override {
    pb_ctrl::ChannelOpenResponse response;
    response.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onMediaChannelSetupRequest(const pb_ms::Setup& /*request*/) override {
    pb_ms::Config response;
    response.set_status(pb_ms::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelSetupResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onMediaChannelStartIndication(const pb_ms::Start& indication) override {
    session_ = indication.session_id();
    channel_->receive(shared_from_this());
  }

  void onMediaChannelStopIndication(const pb_ms::Stop& /*indication*/) override {
    session_ = -1;
    channel_->receive(shared_from_this());
  }

  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer& buffer) override {
    ipc_.queue_audio(ipc_type_, sample_rate_, static_cast<uint8_t>(channels_),
                     static_cast<uint16_t>(bits_),
                     static_cast<uint64_t>(timestamp), buffer.cdata, buffer.size);
    pb_msrc::Ack ack;
    ack.set_session_id(session_);
    ack.set_ack(1);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendMediaAckIndication(ack, std::move(promise));
    channel_->receive(self);
  }

  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override {
    onMediaWithTimestampIndication(0, buffer);
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (e.getCode() != aasdk::error::ErrorCode::OPERATION_ABORTED) {
      log_err("AudioSink", std::string("channel error: ") + e.what());
    }
  }

 private:
  boost::asio::io_service::strand& strand_;
  std::shared_ptr<ChannelBase> channel_;
  pb_msink::AudioStreamType audio_type_;
  uint32_t sample_rate_;
  uint32_t channels_;
  uint32_t bits_;
  uint8_t ipc_type_;
  IpcBridge& ipc_;
  int32_t session_{-1};
};

// ---------------------------------------------------------------------------
// InputSource: pipes touch events from the web client to the phone.
// ---------------------------------------------------------------------------
class InputSource final
    : public aasdk::channel::inputsource::IInputSourceServiceEventHandler,
      public std::enable_shared_from_this<InputSource> {
 public:
  using Channel = aasdk::channel::inputsource::InputSourceService;

  InputSource(boost::asio::io_service::strand& strand,
              std::shared_ptr<Channel> channel, IpcBridge& ipc,
              const HandlerConfig& cfg)
      : strand_(strand), channel_(std::move(channel)), ipc_(ipc), cfg_(cfg) {}

  void start() {
    auto self = shared_from_this();
    strand_.dispatch([self] { self->channel_->receive(self); });
    std::weak_ptr<InputSource> weak = self;
    ipc_.set_touch_callback([weak](double x, double y, int action) {
      if (auto s = weak.lock()) s->dispatchTouch(x, y, action);
    });
  }

  void fillFeatures(pb_ctrl::ServiceDiscoveryResponse& response) const {
    auto* svc = response.add_channels();
    svc->set_id(static_cast<int32_t>(channel_->getId()));
    auto* input = svc->mutable_input_source_service();
    auto* touch = input->add_touchscreen();
    touch->set_width(static_cast<int32_t>(cfg_.video_width));
    touch->set_height(static_cast<int32_t>(cfg_.video_height));
  }

  void onChannelOpenRequest(const pb_ctrl::ChannelOpenRequest& /*request*/) override {
    pb_ctrl::ChannelOpenResponse response;
    response.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onKeyBindingRequest(const pb_msink::KeyBindingRequest& request) override {
    log_info("InputSource",
             "key_binding_request count=" + std::to_string(request.keycodes_size()));
    pb_msink::KeyBindingResponse response;
    response.set_status(static_cast<int32_t>(pb_shared::MessageStatus::STATUS_SUCCESS));
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendKeyBindingResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (e.getCode() != aasdk::error::ErrorCode::OPERATION_ABORTED) {
      log_err("InputSource", std::string("channel error: ") + e.what());
    }
  }

 private:
  void dispatchTouch(double x_norm, double y_norm, int action_int) {
    if (x_norm < 0) x_norm = 0;
    if (x_norm > 1) x_norm = 1;
    if (y_norm < 0) y_norm = 0;
    if (y_norm > 1) y_norm = 1;
    const uint32_t ax = static_cast<uint32_t>(x_norm * cfg_.video_width);
    const uint32_t ay = static_cast<uint32_t>(y_norm * cfg_.video_height);
    pb_input::PointerAction action = pb_input::ACTION_MOVED;
    switch (action_int) {
      case 0: action = pb_input::ACTION_DOWN; break;
      case 1: action = pb_input::ACTION_UP; break;
      default: action = pb_input::ACTION_MOVED; break;
    }
    const uint64_t ts = now_micros();
    auto self = shared_from_this();
    strand_.dispatch([self, ts, ax, ay, action] {
      pb_input::InputReport report;
      report.set_timestamp(ts);
      auto* tev = report.mutable_touch_event();
      tev->set_action(action);
      tev->set_action_index(0);
      auto* p = tev->add_pointer_data();
      p->set_x(ax);
      p->set_y(ay);
      p->set_pointer_id(0);
      auto promise = aasdk::channel::SendPromise::defer(self->strand_);
      promise->then([] {},
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
      self->channel_->sendInputReport(report, std::move(promise));
    });
  }

  boost::asio::io_service::strand& strand_;
  std::shared_ptr<Channel> channel_;
  IpcBridge& ipc_;
  HandlerConfig cfg_;
};

// ---------------------------------------------------------------------------
// SensorSource: minimal sensor data. We acknowledge sensor subscriptions and
// emit a single driving-status / night-mode value so the head unit goes into
// projection mode. Richer streaming would be a polling timer extension.
// ---------------------------------------------------------------------------
class SensorSource final
    : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler,
      public std::enable_shared_from_this<SensorSource> {
 public:
  using Channel = aasdk::channel::sensorsource::SensorSourceService;

  SensorSource(boost::asio::io_service::strand& strand,
               std::shared_ptr<Channel> channel)
      : strand_(strand), channel_(std::move(channel)) {}

  void start() {
    auto self = shared_from_this();
    strand_.dispatch([self] { self->channel_->receive(self); });
  }

  void fillFeatures(pb_ctrl::ServiceDiscoveryResponse& response) const {
    auto* svc = response.add_channels();
    svc->set_id(static_cast<int32_t>(channel_->getId()));
    auto* sensor = svc->mutable_sensor_source_service();
    sensor->add_sensors()->set_sensor_type(
        pb_sensor::SensorType::SENSOR_DRIVING_STATUS_DATA);
    sensor->add_sensors()->set_sensor_type(pb_sensor::SensorType::SENSOR_NIGHT_MODE);
  }

  void onChannelOpenRequest(const pb_ctrl::ChannelOpenRequest& /*request*/) override {
    pb_ctrl::ChannelOpenResponse response;
    response.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onSensorStartRequest(const pb_sensor::SensorRequest& request) override {
    log_info("SensorSource",
             "sensor_start_request type=" + std::to_string(request.type()));
    pb_sensor::SensorStartResponseMessage response;
    response.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    if (request.type() == pb_sensor::SensorType::SENSOR_DRIVING_STATUS_DATA) {
      promise->then([self] { self->sendDriving(); },
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    } else if (request.type() == pb_sensor::SensorType::SENSOR_NIGHT_MODE) {
      promise->then([self] { self->sendNight(); },
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    } else {
      promise->then([] {},
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    }
    channel_->sendSensorStartResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (e.getCode() != aasdk::error::ErrorCode::OPERATION_ABORTED) {
      log_err("SensorSource", std::string("channel error: ") + e.what());
    }
  }

 private:
  void sendDriving() {
    pb_sensor::SensorBatch batch;
    batch.add_driving_status_data()->set_status(
        pb_sensor::DrivingStatus::DRIVE_STATUS_UNRESTRICTED);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendSensorEventIndication(batch, std::move(promise));
  }

  void sendNight() {
    pb_sensor::SensorBatch batch;
    batch.add_night_mode_data()->set_night_mode(false);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendSensorEventIndication(batch, std::move(promise));
  }

  boost::asio::io_service::strand& strand_;
  std::shared_ptr<Channel> channel_;
};

// ---------------------------------------------------------------------------
// Pinger: 5s ping/pong keepalive, modelled after openauto.
// ---------------------------------------------------------------------------
class Pinger final : public std::enable_shared_from_this<Pinger> {
 public:
  using Promise = aasdk::io::Promise<void>;

  Pinger(boost::asio::io_service::strand& strand, int duration_ms)
      : strand_(strand), timer_(strand.context()), duration_ms_(duration_ms) {}

  void ping(Promise::Pointer promise) {
    auto self = shared_from_this();
    auto p = std::move(promise);
    strand_.dispatch([self, p = std::move(p)]() mutable {
      self->cancelled_ = false;
      if (self->promise_) {
        self->promise_->reject(
            aasdk::error::Error(aasdk::error::ErrorCode::OPERATION_IN_PROGRESS));
      } else {
        ++self->pings_;
        self->promise_ = std::move(p);
        self->timer_.expires_from_now(boost::posix_time::milliseconds(self->duration_ms_));
        self->timer_.async_wait(self->strand_.wrap(
            [self](const boost::system::error_code& ec) { self->onTimerExceeded(ec); }));
      }
    });
  }

  void pong() {
    auto self = shared_from_this();
    strand_.dispatch([self] { ++self->pongs_; });
  }

  void cancel() {
    auto self = shared_from_this();
    strand_.dispatch([self] {
      self->cancelled_ = true;
      self->timer_.cancel();
    });
  }

 private:
  void onTimerExceeded(const boost::system::error_code& ec) {
    if (!promise_) return;
    if (ec == boost::asio::error::operation_aborted || cancelled_) {
      promise_->reject(aasdk::error::Error(aasdk::error::ErrorCode::OPERATION_ABORTED));
    } else if (pings_ - pongs_ > 4) {
      promise_->reject(aasdk::error::Error());
    } else {
      promise_->resolve();
    }
    promise_.reset();
  }

  boost::asio::io_service::strand& strand_;
  boost::asio::deadline_timer timer_;
  int duration_ms_;
  bool cancelled_{false};
  int pings_{0};
  int pongs_{0};
  Promise::Pointer promise_;
};

// ---------------------------------------------------------------------------
// ControlHandler: drives the protocol state machine for the control channel.
// ---------------------------------------------------------------------------
class ControlHandler final
    : public aasdk::channel::control::IControlServiceChannelEventHandler,
      public std::enable_shared_from_this<ControlHandler> {
 public:
  using QuitCallback = std::function<void()>;
  using Channel = aasdk::channel::control::ControlServiceChannel;

  ControlHandler(boost::asio::io_service::strand& strand,
                 aasdk::messenger::ICryptor::Pointer cryptor,
                 std::shared_ptr<Channel> channel, IpcBridge& ipc,
                 const HandlerConfig& cfg,
                 std::shared_ptr<VideoSink> video,
                 std::shared_ptr<AudioSink> media_audio,
                 std::shared_ptr<AudioSink> system_audio,
                 std::shared_ptr<InputSource> input,
                 std::shared_ptr<SensorSource> sensor,
                 std::shared_ptr<Pinger> pinger, QuitCallback on_quit)
      : strand_(strand),
        cryptor_(std::move(cryptor)),
        channel_(std::move(channel)),
        ipc_(ipc),
        cfg_(cfg),
        video_(std::move(video)),
        media_audio_(std::move(media_audio)),
        system_audio_(std::move(system_audio)),
        input_(std::move(input)),
        sensor_(std::move(sensor)),
        pinger_(std::move(pinger)),
        on_quit_(std::move(on_quit)) {}

  void start() {
    auto self = shared_from_this();
    strand_.dispatch([self] {
      log_info("Control", "starting handshake");
      self->video_->start();
      self->media_audio_->start();
      if (self->system_audio_) self->system_audio_->start();
      self->input_->start();
      self->sensor_->start();
      self->schedulePing();

      auto promise = aasdk::channel::SendPromise::defer(self->strand_);
      promise->then([] {},
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
      self->channel_->sendVersionRequest(std::move(promise));
      self->channel_->receive(self);
    });
  }

  void stop() {
    auto self = shared_from_this();
    strand_.dispatch([self] {
      log_info("Control", "stop");
      self->pinger_->cancel();
    });
  }

  void onVersionResponse(uint16_t major, uint16_t minor,
                         pb_shared::MessageStatus status) override {
    std::ostringstream msg;
    msg << "version " << major << "." << minor << " status=" << status;
    log_info("Control", msg.str());
    if (status == pb_shared::MessageStatus::STATUS_NO_COMPATIBLE_VERSION) {
      log_err("Control", "version mismatch, quitting");
      triggerQuit();
      return;
    }
    try {
      cryptor_->doHandshake();
      auto self = shared_from_this();
      auto promise = aasdk::channel::SendPromise::defer(strand_);
      promise->then([] {},
                    [self](const aasdk::error::Error& e) { self->onChannelError(e); });
      channel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
      channel_->receive(self);
    } catch (const aasdk::error::Error& e) {
      onChannelError(e);
    }
  }

  void onHandshake(const aasdk::common::DataConstBuffer& payload) override {
    try {
      cryptor_->writeHandshakeBuffer(payload);
      auto self = shared_from_this();
      if (!cryptor_->doHandshake()) {
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([] {},
                      [self](const aasdk::error::Error& e) { self->onChannelError(e); });
        channel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
      } else {
        log_info("Control", "ssl handshake complete");
        pb_ctrl::AuthResponse auth;
        auth.set_status(pb_shared::MessageStatus::STATUS_SUCCESS);
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([] {},
                      [self](const aasdk::error::Error& e) { self->onChannelError(e); });
        channel_->sendAuthComplete(auth, std::move(promise));
      }
      channel_->receive(self);
    } catch (const aasdk::error::Error& e) {
      onChannelError(e);
    }
  }

  void onServiceDiscoveryRequest(const pb_ctrl::ServiceDiscoveryRequest& request) override {
    log_info("Control",
             std::string("service_discovery from ") + request.device_name());

    pb_ctrl::ServiceDiscoveryResponse response;
    response.set_driver_position(pb_ctrl::DriverPosition::DRIVER_POSITION_LEFT);
    response.set_display_name(cfg_.name);
    response.set_probe_for_support(false);

    auto* conn = response.mutable_connection_configuration();
    auto* ping_cfg = conn->mutable_ping_configuration();
    ping_cfg->set_tracked_ping_count(5);
    ping_cfg->set_timeout_ms(3000);
    ping_cfg->set_interval_ms(1000);
    ping_cfg->set_high_latency_threshold_ms(200);

    auto* hu = response.mutable_headunit_info();
    hu->set_make(cfg_.manufacturer);
    hu->set_model(cfg_.model);
    hu->set_year("2026");
    hu->set_vehicle_id("WHUTFA-1");
    hu->set_head_unit_make(cfg_.manufacturer);
    hu->set_head_unit_model(cfg_.name);
    hu->set_head_unit_software_build("1");
    hu->set_head_unit_software_version("0.1");

    video_->fillFeatures(response);
    media_audio_->fillFeatures(response);
    if (system_audio_) system_audio_->fillFeatures(response);
    input_->fillFeatures(response);
    sensor_->fillFeatures(response);

    // Tell the Node server about projection geometry so the web UI can
    // configure WebCodecs ahead of receiving frames.
    {
      std::ostringstream s;
      s << "{\"event\":\"video_config\",\"width\":" << cfg_.video_width
        << ",\"height\":" << cfg_.video_height << ",\"fps\":" << cfg_.video_fps << "}";
      ipc_.emit_control(s.str());
    }
    ipc_.emit_control("{\"event\":\"session\",\"state\":\"active\"}");

    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendServiceDiscoveryResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onAudioFocusRequest(const pb_ctrl::AudioFocusRequest& request) override {
    pb_ctrl::AudioFocusStateType state =
        request.audio_focus_type() == pb_ctrl::AudioFocusRequestType::AUDIO_FOCUS_RELEASE
            ? pb_ctrl::AudioFocusStateType::AUDIO_FOCUS_STATE_LOSS
            : pb_ctrl::AudioFocusStateType::AUDIO_FOCUS_STATE_GAIN;
    pb_ctrl::AudioFocusNotification response;
    response.set_focus_state(state);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendAudioFocusResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onByeByeRequest(const pb_ctrl::ByeByeRequest& request) override {
    log_info("Control", "byebye reason=" + std::to_string(request.reason()));
    pb_ctrl::ByeByeResponse response;
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([self] { self->triggerQuit(); },
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendShutdownResponse(response, std::move(promise));
  }

  void onByeByeResponse(const pb_ctrl::ByeByeResponse& /*response*/) override {
    triggerQuit();
  }

  void onNavigationFocusRequest(
      const pb_ctrl::NavFocusRequestNotification& /*request*/) override {
    pb_ctrl::NavFocusNotification response;
    response.set_focus_type(pb_ctrl::NavFocusType::NAV_FOCUS_PROJECTED);
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendNavigationFocusResponse(response, std::move(promise));
    channel_->receive(self);
  }

  void onVoiceSessionRequest(
      const pb_ctrl::VoiceSessionNotification& /*request*/) override {
    channel_->receive(shared_from_this());
  }

  void onBatteryStatusNotification(
      const pb_ctrl::BatteryStatusNotification& /*notification*/) override {
    channel_->receive(shared_from_this());
  }

  void onPingRequest(const pb_ctrl::PingRequest& /*request*/) override {
    channel_->receive(shared_from_this());
  }

  void onPingResponse(const pb_ctrl::PingResponse& /*response*/) override {
    pinger_->pong();
    channel_->receive(shared_from_this());
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;
    log_err("Control", std::string("channel error: ") + e.what());
    triggerQuit();
  }

 private:
  void schedulePing() {
    auto self = shared_from_this();
    auto promise = Pinger::Promise::defer(strand_);
    promise->then(
        [self] {
          self->sendPing();
          self->schedulePing();
        },
        [self](const aasdk::error::Error& err) {
          if (err.getCode() != aasdk::error::ErrorCode::OPERATION_ABORTED &&
              err.getCode() != aasdk::error::ErrorCode::OPERATION_IN_PROGRESS) {
            log_err("Control", "ping timeout, quitting");
            self->triggerQuit();
          }
        });
    pinger_->ping(std::move(promise));
  }

  void sendPing() {
    pb_ctrl::PingRequest req;
    req.set_timestamp(static_cast<int64_t>(now_micros()));
    auto self = shared_from_this();
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([] {},
                  [self](const aasdk::error::Error& e) { self->onChannelError(e); });
    channel_->sendPingRequest(req, std::move(promise));
  }

  void triggerQuit() {
    if (quit_triggered_.exchange(true)) return;
    if (on_quit_) on_quit_();
  }

  boost::asio::io_service::strand& strand_;
  aasdk::messenger::ICryptor::Pointer cryptor_;
  std::shared_ptr<Channel> channel_;
  IpcBridge& ipc_;
  HandlerConfig cfg_;
  std::shared_ptr<VideoSink> video_;
  std::shared_ptr<AudioSink> media_audio_;
  std::shared_ptr<AudioSink> system_audio_;
  std::shared_ptr<InputSource> input_;
  std::shared_ptr<SensorSource> sensor_;
  std::shared_ptr<Pinger> pinger_;
  QuitCallback on_quit_;
  std::atomic<bool> quit_triggered_{false};
};

}  // namespace

// ============================================================================
// Session::Impl
// ============================================================================

struct Session::Impl {
  Impl(boost::asio::io_service& io, IpcBridge& ipc, const HandlerConfig& cfg)
      : io(io),
        ipc(ipc),
        cfg(cfg),
        video_strand(io),
        media_audio_strand(io),
        system_audio_strand(io),
        input_strand(io),
        sensor_strand(io),
        control_strand(io),
        pinger_strand(io) {}

  boost::asio::io_service& io;
  IpcBridge& ipc;
  HandlerConfig cfg;

  boost::asio::io_service::strand video_strand;
  boost::asio::io_service::strand media_audio_strand;
  boost::asio::io_service::strand system_audio_strand;
  boost::asio::io_service::strand input_strand;
  boost::asio::io_service::strand sensor_strand;
  boost::asio::io_service::strand control_strand;
  boost::asio::io_service::strand pinger_strand;

  aasdk::transport::ITransport::Pointer transport;
  aasdk::messenger::ICryptor::Pointer cryptor;
  aasdk::messenger::IMessenger::Pointer messenger;

  std::shared_ptr<VideoSink> video;
  std::shared_ptr<AudioSink> media_audio;
  std::shared_ptr<AudioSink> system_audio;
  std::shared_ptr<InputSource> input;
  std::shared_ptr<SensorSource> sensor;
  std::shared_ptr<Pinger> pinger;
  std::shared_ptr<ControlHandler> control;

  std::atomic<bool> active{false};
};

Session::Session(boost::asio::io_service& io, IpcBridge& ipc, const HandlerConfig& cfg)
    : impl_(std::make_unique<Impl>(io, ipc, cfg)) {}

Session::~Session() { stop(); }

bool Session::active() const { return impl_ && impl_->active.load(); }

void Session::start(std::shared_ptr<aasdk::usb::IAOAPDevice> aoapDevice) {
  log_info("Session", "starting AA session with AOAP device");
  impl_->ipc.emit_control("{\"event\":\"session\",\"state\":\"connecting\"}");

  impl_->transport =
      std::make_shared<aasdk::transport::USBTransport>(impl_->io, std::move(aoapDevice));
  auto ssl_wrapper = std::make_shared<aasdk::transport::SSLWrapper>();
  impl_->cryptor = std::make_shared<aasdk::messenger::Cryptor>(ssl_wrapper);
  impl_->cryptor->init();

  auto in_stream = std::make_shared<aasdk::messenger::MessageInStream>(
      impl_->io, impl_->transport, impl_->cryptor);
  auto out_stream = std::make_shared<aasdk::messenger::MessageOutStream>(
      impl_->io, impl_->transport, impl_->cryptor);
  impl_->messenger =
      std::make_shared<aasdk::messenger::Messenger>(impl_->io, in_stream, out_stream);

  auto video_channel =
      std::make_shared<aasdk::channel::mediasink::video::channel::VideoChannel>(
          impl_->video_strand, impl_->messenger);
  impl_->video = std::make_shared<VideoSink>(impl_->video_strand, video_channel,
                                             impl_->ipc, impl_->cfg);

  auto media_audio_channel =
      std::make_shared<aasdk::channel::mediasink::audio::channel::MediaAudioChannel>(
          impl_->media_audio_strand, impl_->messenger);
  impl_->media_audio = std::make_shared<AudioSink>(
      impl_->media_audio_strand, media_audio_channel,
      pb_msink::AUDIO_STREAM_MEDIA, impl_->cfg.media_sample_rate, 2, 16,
      /*ipc_type=*/1, impl_->ipc);

  auto system_audio_channel =
      std::make_shared<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>(
          impl_->system_audio_strand, impl_->messenger);
  impl_->system_audio = std::make_shared<AudioSink>(
      impl_->system_audio_strand, system_audio_channel,
      pb_msink::AUDIO_STREAM_SYSTEM_AUDIO, impl_->cfg.speech_sample_rate, 1, 16,
      /*ipc_type=*/2, impl_->ipc);

  auto input_channel = std::make_shared<aasdk::channel::inputsource::InputSourceService>(
      impl_->input_strand, impl_->messenger);
  impl_->input = std::make_shared<InputSource>(impl_->input_strand, input_channel,
                                               impl_->ipc, impl_->cfg);

  auto sensor_channel =
      std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
          impl_->sensor_strand, impl_->messenger);
  impl_->sensor = std::make_shared<SensorSource>(impl_->sensor_strand, sensor_channel);

  impl_->pinger = std::make_shared<Pinger>(impl_->pinger_strand, kPingIntervalMs);

  auto control_channel =
      std::make_shared<aasdk::channel::control::ControlServiceChannel>(
          impl_->control_strand, impl_->messenger);

  std::weak_ptr<Session> weak_self = shared_from_this();
  auto on_quit = [weak_self] {
    auto self = weak_self.lock();
    if (!self) return;
    log_info("Session", "phone signalled quit; tearing down");
    self->impl_->ipc.emit_control("{\"event\":\"session\",\"state\":\"idle\"}");
    self->stop();
  };

  impl_->control = std::make_shared<ControlHandler>(
      impl_->control_strand, impl_->cryptor, control_channel, impl_->ipc, impl_->cfg,
      impl_->video, impl_->media_audio, impl_->system_audio, impl_->input,
      impl_->sensor, impl_->pinger, std::move(on_quit));

  impl_->active = true;
  impl_->control->start();
}

void Session::stop() {
  if (!impl_) return;
  if (!impl_->active.exchange(false)) return;
  log_info("Session", "stop");
  if (impl_->control) impl_->control->stop();
  if (impl_->pinger) impl_->pinger->cancel();
  try {
    if (impl_->messenger) impl_->messenger->stop();
    if (impl_->transport) impl_->transport->stop();
    if (impl_->cryptor) impl_->cryptor->deinit();
  } catch (...) {
    log_err("Session", "exception during stop");
  }
}

}  // namespace whutfa::aa
