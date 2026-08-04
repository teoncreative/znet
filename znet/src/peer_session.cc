//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/peer_session.h"
#include "znet/scheduler.h"
#include "znet/error.h"

#include <atomic>
#include <utility>

namespace znet {

namespace {

// sessions are minted on whichever thread accepted or dialed them, so this
// counter is shared across threads. A function rather than a local static in
// the constructor body, so the id exists in time for the member init list.
SessionId NextSessionId() {
  static std::atomic<SessionId> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

PeerSession::PeerSession(std::shared_ptr<InetAddress> local_address,
                         std::shared_ptr<InetAddress> remote_address,
                         std::unique_ptr<TransportLayer> transport_layer,
                         ConnectionType connection_type,
                         bool is_initiator,
                         bool self_managed,
                         const SessionOptions& options)
    : id_(NextSessionId()),
      local_address_(std::move(local_address)),
      remote_address_(std::move(remote_address)),
      connection_type_(connection_type),
      transport_layer_(std::move(transport_layer)),
      pipeline_(encryption_layer_, id_),
      encryption_layer_(*this),
      options_(options),
      is_initiator_(is_initiator),
      connect_time_(std::chrono::steady_clock::now()),
      // the parameter, not options_, so this does not depend on declaration
      // order between the two
      outbound_(options.common.send_queue_capacity) {
  pipeline_.SetCompressionThreshold(options_.common.compression_threshold);
  pipeline_.SetDumpOnDecodeFailure(options_.common.dump_on_decode_failure);
  // only the accepting side's options count; an initiator adopts whatever the
  // server announces at handshake
  negotiated_compression_ =
      is_initiator ? CompressionType::None
                   : ResolveCompressionType(options_.common.compression);
  encryption_layer_.Initialize(is_initiator, options_.common.encryption);
  if (self_managed) {
    task_.Run([this]() {
      while (IsAlive() && !task_.IsStopRequested()) {
        // an idle pass earns a nap: spinning would pin a core per session,
        // and a millisecond of doze is inside any game's tick
        if (!Process()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    });
  }
}

PeerSession::~PeerSession() {
  Close();
  // A self-managed session runs Process() on task_, which touches almost every
  // member declared after it. Leaving the join to ~Task would run it once those
  // members are already destroyed; the destructor body runs first, so stopping
  // here is in time.
  task_.RequestStop();
  task_.Wait();
}

bool PeerSession::Process() {
  if (!IsAlive()) {
    return false;
  }
  if (IsExpired()) {
    ZNET_LOG_INFO("Session {} was expired!", id_);
    Close();
    return true;
  }
  transport_layer_->Update();
  bool worked = false;
  // drain what is already buffered rather than one message per tick, otherwise
  // throughput is capped at the caller's tick rate. The bound keeps one busy
  // session from starving the others sharing this worker.
  std::shared_ptr<Buffer> buffer;
  for (uint32_t i = 0; i < kMaxReceivesPerProcess; i++) {
    buffer = transport_layer_->Receive();
    if (!buffer) {
      break;
    }
    worked = true;
    buffer = pipeline_.Decode(std::move(buffer));
    if (!buffer) {
      continue;
    }
    if (handler_ && pipeline_.has_codec()) {
      ZNET_METRIC(metrics_.common.messages_received++);
      ZNET_METRIC(metrics_.common.message_bytes_received += buffer->size());
      DecodeStats stats = pipeline_.Dispatch(buffer, *handler_);
      if (stats.invalid_frames > 0) {
        invalid_frames_ += stats.invalid_frames;
        ZNET_METRIC(metrics_.common.invalid_frames += stats.invalid_frames);
        const uint32_t limit = options_.common.max_invalid_frames;
        if (limit != 0 && invalid_frames_ >= limit) {
          ZNET_LOG_WARN("Session {} reached {} undecodable frames, closing.",
                        id_, invalid_frames_);
          CloseOptions close_options;
          close_options.Set<NoLingerKey>(true);
          Close(close_options);
          break;
        }
      }
    }
  }
  // handlers above almost always answer, and Update() already ran, so without
  // this their replies would wait out a tick and every round trip would cost
  // two. A dead session drains anyway, to release what it queued rather than
  // hold it until destruction. Who encodes is OutboundQueue's rule.
  if (outbound_.ShouldEncodeInline() || !IsAlive()) {
    worked = DrainOutbound() || worked;
  }
  if (IsAlive()) {
    transport_layer_->Flush();
  }
  return worked;
}

Result PeerSession::Close(CloseOptions options) {
  if (!transport_layer_) {
    return Result::InvalidTransport;
  }
  return transport_layer_->Close(options);
}

bool PeerSession::IsAlive() {
  return transport_layer_ && !transport_layer_->IsClosed();
}

void PeerSession::Ready() {
  if (!IsAlive()) {
    return;
  }
  connect_time_ = std::chrono::steady_clock::now();
  if (negotiated_compression_ != CompressionType::None) {
    SetOutCompression(negotiated_compression_);
  }
  // last, and with release: the codec, the compression type and the derived
  // keys are all written above, and this publishes them. SendPacket() refuses
  // until it sees this.
  is_ready_.store(true, std::memory_order_release);
}

bool PeerSession::EncodeAndSend(const std::shared_ptr<Packet>& packet,
                                SendOptions options) {
  // the transport decides what "in order relative to each other" means for
  // these options, and the cipher's sequence has to be scoped the same way
  auto buffer =
      pipeline_.Encode(packet, transport_layer_->OrderingDomain(options));
  if (!buffer) {
    return false;
  }
  ZNET_METRIC(metrics_.common.message_bytes_sent += buffer->readable_bytes());
  if (!transport_layer_->Send(buffer, options)) {
    ZNET_METRIC(metrics_.common.send_failures++);
    return false;
  }
  ZNET_METRIC(metrics_.common.messages_sent++);
  return true;
}

bool PeerSession::SendImmediate(std::shared_ptr<Packet> packet,
                                SendOptions options) {
  if (!packet || !IsAlive()) {
    return false;
  }
  // Encodes without taking the claim, which is only safe because this runs
  // during the handshake: SendPacket() refuses until IsReady(), so outbound_ is
  // empty and no drain can be encoding concurrently. Assert it rather than
  // leave it to be discovered.
  assert(!is_ready_.load(std::memory_order_acquire) &&
         "SendImmediate is handshake-only; use SendPacket once ready");
  return EncodeAndSend(packet, options);
}

bool PeerSession::SendPacket(std::shared_ptr<Packet> packet,
                             SendOptions options) {
  // the ready gate is what makes the encode path safe to read unguarded, so
  // this refuses rather than queueing and hoping.
  if (!packet || !IsReady() || !IsAlive()) {
    return false;
  }
  // no lock, no allocation and no encoding below: this runs on the
  // application's thread and must not block on a worker.
  if (!outbound_.Push(std::move(packet), options)) {
    // debug, not a warning: nothing was lost and the caller has been told to
    // try again, so a caller pushing against a full queue would otherwise turn
    // its own backpressure into a log flood.
    ZNET_LOG_DEBUG("Session {} outbound queue is full ({}), refusing the send.",
                   id_, outbound_.capacity());
    return false;
  }
  return true;
}

bool PeerSession::DrainOutbound() {
  return outbound_.Drain([this](OutboundQueue::Item& item) {
    if (!IsAlive()) {
      return false;  // keep draining, so a dead session releases what it holds
    }
    EncodeAndSend(item.packet, item.options);
    return true;
  });
}


}