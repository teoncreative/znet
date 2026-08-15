//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//
// ZDT client and server backends: socket lifetime, the offline handshake, the
// receive threads and (server side) demultiplexing datagrams to per-peer
// transports.
//

#ifndef ZNET_BACKENDS_ZDT_ZDT_BACKENDS_H_
#define ZNET_BACKENDS_ZDT_ZDT_BACKENDS_H_

#include "znet/admission.h"
#include "znet/backends/backend.h"
#include "znet/buffer.h"
#include "znet/inet_addr.h"
#include "znet/metrics.h"
#include "znet/mpsc_queue.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/compat.h"
#include "znet/transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "znet/backends/zdt/zdt_domain.h"
#include "znet/backends/zdt/zdt_transport.h"

namespace znet {
namespace backends {

class ZDTClientBackend : public ClientBackend {
 public:
  explicit ZDTClientBackend(std::shared_ptr<InetAddress> server_address,
                            const SessionOptions& options = {});
  ~ZDTClientBackend() override;
  ZDTClientBackend(const ZDTClientBackend&) = delete;

  Result Bind() override;
  Result Bind(const std::string& ip, PortNumber port) override;

 private:
  // shared by both Bind() overloads: open, configure, bind, record the address
  Result BindTo(const InetAddress& address);

 public:
  Result Connect() override;
  Result Close() override;
  void Update() override;
  bool IsAlive() override;
  void SetWakeCallback(std::function<void()> on_data) override {
    on_data_ = std::move(on_data);
  }
  void StopReceiving() override;
  bool DrivesOwnReceive() const override { return true; }

  std::shared_ptr<PeerSession> client_session() override { return client_session_; }
  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

  void ReleaseSession() override {
    if (client_session_ && !client_session_->IsAlive()) {
      client_session_ = nullptr;
    }
  }

 private:
  // on success fills `out` with the negotiated connection and returns Result::Success; otherwise a
  // granular failure Result (IncompatibleVersion, ServerFull, Timeout, ...).
  Result Handshake(ZDTConnection& out);

  // like the server's, so an arriving datagram is seen at once rather than on
  // the client loop's next tick. started only after the handshake, which reads
  // the socket directly and would otherwise race it.
  void ReceiveLoop();

  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<UDPSocket> socket_;
  std::shared_ptr<ZDTInbox> inbox_;
  std::thread receive_thread_;
  std::mutex receive_thread_mutex_;
  std::atomic_bool receiving_{false};
  std::function<void()> on_data_;
  std::shared_ptr<PeerSession> client_session_;
  ZDTOptions config_;
  SessionOptions session_options_;  // passed to the PeerSession it creates
  uint64_t guid_ = 0;
  bool is_bind_ = false;
};

class ZDTServerBackend : public ServerBackend {
 public:
  explicit ZDTServerBackend(std::shared_ptr<InetAddress> bind_address,
                            const SessionOptions& child_options = {},
                            const ServerOptions& server_options = {});
  ~ZDTServerBackend() override;
  ZDTServerBackend(const ZDTServerBackend&) = delete;

  Result Bind() override;
  Result Listen() override;
  Result Close() override;
  void Update() override;

  std::shared_ptr<PeerSession> Accept() override;
  void AcceptAndReject() override;
  bool IsAlive() override;


  void SetWakeCallback(std::function<void()> on_data) override {
    on_data_ = std::move(on_data);
  }

  void StopReceiving() override;

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

  ServerMetrics metrics() const override {
    // the receive thread writes these counters, so sample under the lock
    std::lock_guard<std::mutex> lock(state_mutex_);
    ServerMetrics out = metrics_;
    out.connection_type = ConnectionType::ZDT;
    return out;
  }

 private:
  struct Route {
    std::weak_ptr<PeerSession> session;
    std::shared_ptr<ZDTInbox> inbox;
    std::shared_ptr<InetAddress> peer;
    uint64_t remote_guid = 0;
  };

  // body of the receive thread: blocks in recvfrom and routes each datagram as
  // it lands. Online -> the matching peer's inbox; offline -> the stateless
  // handshake path (which may create a session and push it onto
  // pending_accept_). Returns when is_listening_ goes false.
  void ReceiveLoop();
  void RouteDatagram(Buffer& datagram,
                     const std::shared_ptr<InetAddress>& from);
  void HandleOffline(Buffer& buffer, const std::shared_ptr<InetAddress>& from,
                     size_t datagram_size);
  void MaybeRotateSecret();
  ZDTCookie CookieFor(const std::string& peer_readable, uint32_t epoch) const;
  // per-source handshake rate limit (bounded, self-pruning). returns false when
  // the source has exceeded per_source_handshake_rate this second.
  bool AllowHandshake(const std::string& peer_readable);

  // makes Close() a single winner, so two threads stopping the server together
  // do not both tear the tables and the socket down. Guards nothing else; the
  // receive thread and the server's tick share state_mutex_ instead.
  std::mutex mutex_;
  std::shared_ptr<InetAddress> bind_address_;
  std::shared_ptr<UDPSocket> socket_;
  ZDTOptions config_;
  SessionOptions child_session_options_;  // passed to each accepted PeerSession
  // touched only from the offline-datagram path, like source_rate_
  AdmissionControl admission_;
  std::atomic_bool is_bind_{false};
  std::atomic_bool is_listening_{false};

  struct SourceRate {
    int count = 0;
    std::chrono::steady_clock::time_point window_start;
  };

  // routes_, pending_accept_, source_rate_, the cookie secrets and metrics_ are
  // written by the receive thread and read by the Server's tick, so they need a
  // lock. Deliberately not mutex_: the Server holds that across a whole tick,
  // and stalling the receive thread that long is what overflows the socket.
  mutable std::mutex state_mutex_;
  // set once before the receive thread starts and never reassigned, so the
  // thread can read it without synchronizing.
  std::function<void()> on_data_;
  // StopReceiving() is reachable both from the shutdown path and from Close()
  // on another thread. Joining the same thread twice is undefined, so entry is
  // serialized here.
#ifndef NDEBUG
  // RouteDatagram and everything it reaches (HandleOffline, the cookie secrets,
  // the rate limiter) belong to the receive thread.
  ThreadDomain receive_domain_;
#endif
  std::mutex receive_thread_mutex_;
  std::thread receive_thread_;
  std::atomic_bool receiving_{false};

  std::unordered_map<std::string, Route> routes_;
  std::deque<std::shared_ptr<PeerSession>> pending_accept_;
  std::unordered_map<std::string, SourceRate> source_rate_;

  // cookie signing secrets (touched only on the receive thread).
  std::array<uint8_t, 32> secret_current_{};
  std::array<uint8_t, 32> secret_previous_{};
  uint32_t epoch_ = 0;
  bool has_previous_secret_ = false;
  std::chrono::steady_clock::time_point last_rotation_;
  uint64_t server_guid_ = 0;
  ServerMetrics metrics_;
};

}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_BACKENDS_H_
