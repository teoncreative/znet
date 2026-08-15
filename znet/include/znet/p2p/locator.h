//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_P2P_LOCATOR_H_
#define ZNET_P2P_LOCATOR_H_

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/compat.h"
#include "znet/event.h"
#include "znet/p2p/host.h"

#include <vector>

namespace znet {
namespace p2p {

ZNET_INLINE_CONSTEXPR uint64_t kInvalidPunchId =
    (std::numeric_limits<uint64_t>::max)();

/** @brief Everything a PeerLocator is constructed from. */
struct PeerLocatorConfig {
  std::string server_address;
  PortNumber server_port = 0;
  // no transport choice here: the relay link is TCP, and the punch transport
  // is decided by the rendezvous server so both peers agree
};

/** @brief Which stage of finding a peer failed. */
enum class PeerLocatorPhase : uint8_t {
  Relay,       /**< The link to the rendezvous server. */
  Rendezvous,  /**< The name exchange on it. */
  Punch,       /**< The hole punch itself. */
};

inline std::string GetPeerLocatorPhaseString(PeerLocatorPhase phase) {
  switch (phase) {
    case PeerLocatorPhase::Relay:
      return "Relay";
    case PeerLocatorPhase::Rendezvous:
      return "Rendezvous";
    case PeerLocatorPhase::Punch:
      return "Punch";
    default:
      return "Unknown";
  }
}

class PeerLocatorReadyEvent : public Event {
 public:
  PeerLocatorReadyEvent(std::string peer_name,
                        std::shared_ptr<InetAddress> endpoint)
      : peer_name_(std::move(peer_name)), endpoint_(std::move(endpoint)) {}

  ZNET_NODISCARD const std::string& peer_name() const { return peer_name_; }

  /** @brief This peer's address as the rendezvous observed it: the public
   * mapping other peers will be told to punch. */
  ZNET_NODISCARD std::shared_ptr<InetAddress> endpoint() const {
    return endpoint_;
  }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorReadyEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class PeerLocatorCloseEvent : public Event {
 public:
  PeerLocatorCloseEvent() = default;

  ZNET_EVENT_CLASS_TYPE(PeerLocatorCloseEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
};

/**
 * @brief Something on the way to a peer failed, and this is what and why.
 *
 * A Rendezvous failure (an unknown peer name) leaves the relay link up, so
 * AskPeer can simply be called again. Relay and Punch failures are followed
 * by PeerLocatorCloseEvent, the terminal "no session is coming" signal.
 */
class PeerLocatorFailedEvent : public Event {
 public:
  PeerLocatorFailedEvent(PeerLocatorPhase phase, Result reason,
                         std::string target_peer)
      : phase_(phase), reason_(reason), target_peer_(std::move(target_peer)) {}

  ZNET_NODISCARD PeerLocatorPhase phase() const { return phase_; }

  ZNET_NODISCARD Result reason() const { return reason_; }

  /** @brief The peer being sought; empty when none was involved yet. */
  ZNET_NODISCARD const std::string& target_peer() const { return target_peer_; }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorFailedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  PeerLocatorPhase phase_;
  Result reason_;
  std::string target_peer_;
};

class PeerConnectedEvent : public Event {
 public:
  explicit PeerConnectedEvent(std::shared_ptr<PeerSession> session,
                              uint64_t punch_id, std::string self_peer_name,
                              std::string target_peer_name)
      : session_(session),
        punch_id_(punch_id),
        self_peer_name_(self_peer_name),
        target_peer_name_(target_peer_name) {}

  ZNET_NODISCARD std::shared_ptr<PeerSession> session() const {
    return session_;
  }

  ZNET_NODISCARD uint64_t punch_id() const { return punch_id_; }

  ZNET_NODISCARD const std::string& self_peer_name() const {
    return self_peer_name_;
  }

  ZNET_NODISCARD const std::string& target_peer_name() const {
    return target_peer_name_;
  }

  ZNET_EVENT_CLASS_TYPE(PeerConnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::shared_ptr<PeerSession> session_;
  uint64_t punch_id_;
  std::string self_peer_name_;
  std::string target_peer_name_;
};

/**
 * @brief The one-shot two-player path: connect to the rendezvous, ask for one
 *        peer, punch, done. The only path that can punch TCP.
 *
 * Every event fires on an internal worker thread; treat the callback like a
 * packet handler and keep it quick. For three or more players, or repeated
 * asks over one socket, use MeshLocator.
 */
class PeerLocator {
 public:
  explicit PeerLocator(const PeerLocatorConfig& config);
  PeerLocator(const PeerLocator&) = delete;
  ~PeerLocator();

  Result Connect();
  Result Disconnect();

  void Wait();

  Result AskPeer(std::string peer_name);

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD std::string peer_name() const;

 private:
  friend class LocatorPacketHandler;

  void OnEvent(Event&);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);
  bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event);
  bool OnConnectionFailedEvent(ClientConnectionFailedEvent& event);

  void SetPeerName(std::string peer_name,
                   std::shared_ptr<InetAddress> endpoint);
  void OnPeerNotFound(const std::string& target_peer);
  void FireFailed(PeerLocatorPhase phase, Result reason,
                  const std::string& target_peer);

  EventCallbackFn event_callback_;
  Client client_;

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
  std::shared_ptr<PeerSession> session_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  Task task_;

  std::shared_ptr<InetAddress> target_endpoint_;
  std::shared_ptr<InetAddress> target_private_endpoint_;
  ConnectionType connection_type_ = ConnectionType::ZDT;
  std::string target_peer_name_;
  uint64_t punch_id_ = kInvalidPunchId;

  bool is_running_ = false;
  bool wake_ = false;  // guarded by mutex_
};

/**
 * @brief Stays on the rendezvous and brokers any number of punches through
 *        one shared-socket Host: the mesh flavor of PeerLocator, for games
 *        with three or more players. ZDT only, so run the rendezvous with
 *        punch type zdt.
 *
 * Events, through the one callback like everything else in znet:
 * - PeerLocatorReadyEvent when the relay names this peer
 * - PeerConnectedEvent once per punched peer, fired on the host's thread,
 *   which is where the session's codec and handler belong
 * - PeerLocatorFailedEvent with the phase and reason on any failure
 * - PeerLocatorCloseEvent when the relay link ends
 *
 * AskPeer may be called any number of times and punches overlap freely.
 * Losing the relay ends matchmaking, not the mesh: punched sessions live
 * until Disconnect() (or their own idle timers) close them, and Connect()
 * may be called again to rejoin the relay with the mesh intact.
 */
class MeshLocator {
 public:
  struct Config {
    std::string server_address;
    PortNumber server_port = 0;
    /** @brief Options every punched session is built with. */
    SessionOptions session_options;
    /** @brief Local address of the shared punch socket. The defaults bind
     * every interface on a system-picked port. */
    std::string bind_address = "0.0.0.0";
    PortNumber bind_port = 0;
  };

  explicit MeshLocator(const Config& config);
  MeshLocator(const MeshLocator&) = delete;
  ~MeshLocator();

  Result Connect();

  /** @brief Leaves the mesh: the relay link, pending punches and every
      punched session all end. */
  Result Disconnect();

  Result AskPeer(std::string peer_name);

  /** @brief Blocks until the relay link ends. The mesh may outlive it. */
  void Wait();

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD std::string peer_name() const;

  /** @brief The socket everything punches from, e.g. for session_count(). */
  ZNET_NODISCARD const Host& host() const { return host_; }

 private:
  friend class MeshLocatorPacketHandler;

  void OnEvent(Event& event);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);
  bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event);
  bool OnConnectionFailedEvent(ClientConnectionFailedEvent& event);
  void SetPeerName(std::string peer_name,
                   std::shared_ptr<InetAddress> endpoint);
  void OnPeerNotFound(const std::string& target_peer);
  void OnPunchRequest(std::string target_peer,
                      std::vector<std::shared_ptr<InetAddress>> candidates,
                      uint64_t punch_id, ConnectionType connection_type);
  void FireFailed(PeerLocatorPhase phase, Result reason,
                  const std::string& target_peer);

  Config config_;
  Host host_;
  Client client_;
  EventCallbackFn event_callback_;
  mutable std::mutex mutex_;
  std::string peer_name_;
  std::shared_ptr<PeerSession> relay_session_;
  bool is_running_ = false;
};

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_LOCATOR_H_
