//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_P2P_RENDEZVOUS_SERVER_H_
#define ZNET_P2P_RENDEZVOUS_SERVER_H_

#include "znet/p2p/rendezvous.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/task.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace znet {
namespace p2p {

/**
 * @brief The rendezvous broker: names peers, observes their public endpoints
 *        and pairs mutual connect requests into punch requests.
 *
 * Instantiable, so it can run inside a larger process or a test as easily as
 * in the standalone rendezvous-server binary. The relay link is always TCP;
 * `punch_connection_type` is the transport the punched peer-to-peer
 * connection will use, decided here so both peers always agree.
 */
class RendezvousServer {
 public:
  struct Config {
    std::string bind_ip = "0.0.0.0";
    PortNumber bind_port = 5001;
    ConnectionType punch_connection_type = ConnectionType::ZDT;
    /**
     * @brief Listener options for the relay itself: allow/deny lists, the
     *        per-source connection throttle and max_connections all apply.
     *        See ServerOptions.
     */
    ServerOptions options;
    /**
     * @brief Locator requests one client may make per request_window;
     *        identify and connect-peer both count. Beyond it the client is
     *        disconnected: a spammer costs a reconnect, not the pairing
     *        thread. Zero disables it.
     */
    uint32_t max_requests_per_window = 30;
    /** @brief The window max_requests_per_window is counted over. */
    std::chrono::milliseconds request_window{10000};
  };

  explicit RendezvousServer(const Config& config);
  ~RendezvousServer();
  RendezvousServer(const RendezvousServer&) = delete;

  /** @brief Binds, listens and starts the pairing thread. */
  Result Start();

  void Stop();

  /** @brief Blocks until the relay stops. */
  void Wait();

  /** @brief Resolved after Start(), so a bind_port of 0 can be read back. */
  ZNET_NODISCARD std::shared_ptr<InetAddress> bind_address() const {
    return server_.bind_address();
  }

 private:
  friend class RendezvousPacketHandler;

  // one connected client, as the pairing thread sees it. All fields are
  // guarded by mutex_: packet handlers run on the relay's session workers.
  struct ClientData {
    std::shared_ptr<PeerSession> session;
    std::string peer_name;
    // every name this client is currently asking for. A set, not a single
    // slot: a mesh client asks for several peers, and one slot made each ask
    // clobber the previous one. An entry is consumed when its pair forms.
    std::set<std::string> pending_targets;
    // the client's claimed private address, relayed to its match as a second
    // punch candidate; null when it did not report one
    std::shared_ptr<InetAddress> private_endpoint;
    // the port the client punches from; zero falls back to the observed one
    PortNumber punch_port = 0;
    std::chrono::steady_clock::time_point request_window_start;
    uint32_t request_count = 0;
  };

  void OnEvent(Event& event);
  bool OnConnectEvent(IncomingClientConnectedEvent& event);
  bool OnDisconnectEvent(IncomingClientDisconnectedEvent& event);
  /** @brief Counts one request; false means over the limit. Caller holds
      mutex_. */
  bool AllowRequest(ClientData& data);
  void PairingLoop();
  void AssignName(const std::shared_ptr<PeerSession>& session);
  void TryPair(const std::shared_ptr<PeerSession>& session,
               const std::string& target);
  std::string GenerateUniqueName();

  Config config_;
  Server server_;
  Task pairing_task_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::deque<std::shared_ptr<PeerSession>> name_await_queue_;
  // each entry names the target it asked for at the time; reading the
  // client's latest state instead would let a newer ask rewrite older ones
  std::deque<std::pair<std::shared_ptr<PeerSession>, std::string>>
      connect_peer_queue_;
  std::deque<std::string> clear_queue_;
  // written only on the pairing thread, under mutex_ like the rest
  std::unordered_map<std::string, std::shared_ptr<ClientData>> registry_;
  std::mt19937_64 punch_id_rng_;
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_RENDEZVOUS_SERVER_H_
