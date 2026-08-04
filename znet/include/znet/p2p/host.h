//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_P2P_HOST_H_
#define ZNET_P2P_HOST_H_

#include "znet/backends/zdt/zdt_connection.h"
#include "znet/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/task.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {
class UDPSocket;
class ZDTTransportLayer;
}  // namespace backends

namespace p2p {

/**
 * @brief One UDP socket carrying every punched peer: the punches in flight
 *        and the sessions they become.
 *
 * A NAT hands out one public mapping per local socket, so every peer of a
 * mesh must be reached through the same socket. The host owns it, runs the
 * punch state machines on it (asynchronously; StartPunch never blocks), and
 * drives the resulting sessions on one thread the way a server worker does.
 * ZDT only: a TCP mesh would need one simultaneous open per peer from the
 * same port, which is not worth what stacks make of it.
 *
 * See the wiki's Peer-to-Peer page for the full flow; tests/p2p_host.cc is a
 * working three-player mesh.
 */
class Host {
 public:
  /**
   * @brief Runs on the host's thread when a punch resolves: Success with the
   *        session, or a failure with null. Setting the session's codec and
   *        handler inside the callback is the intended pattern, exactly like
   *        a connected event.
   */
  using PunchCallback =
      std::function<void(Result, std::shared_ptr<PeerSession>)>;

  struct Config {
    std::string bind_ip = "0.0.0.0";
    /** @brief Zero picks an ephemeral port; read it back with punch_port(). */
    PortNumber bind_port = 0;
    /** @brief Options every punched session is built with. */
    SessionOptions session_options;
  };

  explicit Host(const Config& config);
  ~Host();
  Host(const Host&) = delete;

  /** @brief Opens and binds the socket and starts the tick thread. */
  Result Start();

  /**
   * @brief Fails outstanding punches (Result::AlreadyStopped), closes every
   *        punched session and releases the socket.
   */
  void Stop();

  /**
   * @brief Begins one asynchronous punch.
   *
   * @param candidates the same peer at its different addresses, typically
   *        public then private; whichever answers first wins.
   * @param punch_id the rendezvous-issued id, fed to IsInitiator by callers
   *        that need the tiebreak; the host itself only reports it back.
   * @param is_initiator exactly one of the two peers must pass true.
   */
  void StartPunch(std::vector<std::shared_ptr<InetAddress>> candidates,
                  uint64_t punch_id, bool is_initiator,
                  std::chrono::milliseconds timeout, PunchCallback on_done);

  /** @brief The UDP port every punch and session speaks from. */
  ZNET_NODISCARD PortNumber punch_port() const {
    return local_address_ ? local_address_->port() : 0;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const {
    return local_address_;
  }

  /** @brief Live punched sessions. Approximate off the host thread. */
  ZNET_NODISCARD size_t session_count() const {
    return session_count_.load(std::memory_order_relaxed);
  }

 private:
  struct Punch {
    std::vector<std::shared_ptr<InetAddress>> candidates;
    bool is_initiator = false;
    uint64_t punch_id = 0;
    backends::ZDTConnection connection;
    std::chrono::steady_clock::time_point deadline;
    std::chrono::steady_clock::time_point last_punch;
    std::chrono::steady_clock::time_point last_request;
    PunchCallback on_done;
  };

  struct Route {
    // owned by the session; valid exactly as long as the session lives
    backends::ZDTTransportLayer* transport = nullptr;
    std::shared_ptr<PeerSession> session;
  };

  void TickLoop();
  bool DrainSocket();
  bool TickPunches();
  bool ProcessSessions();
  void HandleOffline(const std::shared_ptr<InetAddress>& from,
                     const uint8_t* data, size_t len);
  // consumes punches_[index]; `first_datagram` is fed to the new transport
  // when the punch completed on an online datagram rather than a Reply2
  void CompletePunch(size_t index, const std::shared_ptr<InetAddress>& from,
                     const uint8_t* first_datagram, size_t len);
  void FailPunch(size_t index, Result reason);

  Config config_;
  std::shared_ptr<backends::UDPSocket> socket_;
  std::shared_ptr<InetAddress> local_address_;
  Task task_;
  std::atomic<bool> running_{false};

  // handoff from StartPunch (any thread) to the tick thread
  std::mutex pending_mutex_;
  std::vector<Punch> pending_;

  // tick thread only
  std::vector<Punch> punches_;
  std::unordered_map<std::string, Route> routes_;
  std::atomic<size_t> session_count_{0};
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_HOST_H_
