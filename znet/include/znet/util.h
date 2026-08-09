
//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_UTIL_H_
#define ZNET_UTIL_H_

#include "znet/precompiled.h"
#include "znet/types.h"

#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>

#ifndef ZNET_TARGET_WIN
#include <poll.h>
#endif

#define ZNET_BIND_FN(fn)                                    \
  [this](auto&&... args) -> decltype(auto) {                \
    return this->fn(std::forward<decltype(args)>(args)...); \
  }

#define ZNET_BIND_GLOBAL_FN(fn)                       \
  [](auto&&... args) -> decltype(auto) {              \
    return fn(std::forward<decltype(args)>(args)...); \
  }

namespace znet {

template<class...>
using void_t = void;

template <class T>
std::string ToHex(const T& numValue, int width) {
  std::ostringstream stream;
  stream << "0x"
         << std::setfill('0') << std::setw(width)
         << std::hex << +numValue;
  return stream.str();
}

std::string GeneratePeerName();

inline bool IsValidSocketHandle(SocketHandle handle) {
#ifdef ZNET_TARGET_WIN
  return handle != kSocketInvalid;
#else
  return handle >= 0;
#endif
}

inline bool CloseSocket(SocketHandle socket) {
  if (IsValidSocketHandle(socket)) {
#ifdef ZNET_TARGET_WIN
    return closesocket(socket) == 0;
#else
    return close(socket) == 0;
#endif
  }
  return false;
}

/**
 * @brief Half-closes both directions on the socket.
 *
 * Used to wake a blocked recv() on a socket being torn down from another
 * thread: plain CloseSocket() does not interrupt an in-progress recv on
 * POSIX, which stalls graceful shutdown.
 */
inline bool ShutdownSocket(SocketHandle socket) {
  if (!IsValidSocketHandle(socket)) {
    return false;
  }
#ifdef ZNET_TARGET_WIN
  return shutdown(socket, SD_BOTH) == 0;
#else
  return shutdown(socket, SHUT_RDWR) == 0;
#endif
}

/**
 * @brief Half-closes the receive direction only.
 *
 * Wakes a recv() parked on this socket without stopping sends on it, which is
 * what a listener needs during shutdown: the receive loop must come out of
 * recvfrom() at once, but the sessions still on that socket have FINs of their
 * own to put out first.
 */
inline bool ShutdownSocketRead(SocketHandle socket) {
  if (!IsValidSocketHandle(socket)) {
    return false;
  }
#ifdef ZNET_TARGET_WIN
  return shutdown(socket, SD_RECEIVE) == 0;
#else
  return shutdown(socket, SHUT_RD) == 0;
#endif
}

namespace detail {

/**
 * @brief Caps a transfer length at what one platform call can express.
 *
 * Winsock's length parameter is an int, so a plain cast of anything above
 * INT_MAX wraps negative and the call fails. Capping instead makes it a short
 * transfer, which every caller already handles by looping or re-polling.
 */
inline size_t SocketIoLength(size_t len) {
#ifdef ZNET_TARGET_WIN
  constexpr size_t kMax = static_cast<size_t>((std::numeric_limits<int>::max)());
  return len > kMax ? kMax : len;
#else
  return len;
#endif
}

}  // namespace detail

//
// send/recv with one signature: a size_t length and an ssize_t result, the
// POSIX shape. Winsock spells both as int and takes char* rather than void*,
// so without these every call site carries its own ifdef around what is one
// operation. Flags are not a parameter: the only one znet ever wants is the
// SIGPIPE suppression below, which is a platform detail rather than a
// caller's choice.
//

/**
 * @brief Sends over a connected socket. @return bytes sent, or -1 on error.
 */
inline ssize_t SocketSend(SocketHandle socket, const void* data, size_t len) {
  const size_t length = detail::SocketIoLength(len);
#ifdef ZNET_TARGET_WIN
  return send(socket, static_cast<const char*>(data), static_cast<int>(length),
              0);
#elif defined(MSG_NOSIGNAL)
  // a peer that reset the connection turns further sends into EPIPE errors
  // instead of a process-killing SIGPIPE. Apple has no such flag; the
  // per-socket SO_NOSIGPIPE is set at construction instead.
  return send(socket, data, length, MSG_NOSIGNAL);
#else
  return send(socket, data, length, 0);
#endif
}

/**
 * @brief Receives from a connected socket. @return bytes read, 0 on an orderly
 *        shutdown by the peer, or -1 on error.
 */
inline ssize_t SocketRecv(SocketHandle socket, void* data, size_t len) {
  const size_t length = detail::SocketIoLength(len);
#ifdef ZNET_TARGET_WIN
  return recv(socket, static_cast<char*>(data), static_cast<int>(length), 0);
#else
  return recv(socket, data, length, 0);
#endif
}

/** @brief Sends one datagram. @return bytes sent, or -1 on error. */
inline ssize_t SocketSendTo(SocketHandle socket, const void* data, size_t len,
                            const sockaddr* addr, socklen_t addr_len) {
  const size_t length = detail::SocketIoLength(len);
#ifdef ZNET_TARGET_WIN
  return sendto(socket, static_cast<const char*>(data),
                static_cast<int>(length), 0, addr, addr_len);
#else
  return sendto(socket, data, length, 0, addr, addr_len);
#endif
}

/** @brief Receives one datagram. @return bytes read, or -1 on error. */
inline ssize_t SocketRecvFrom(SocketHandle socket, void* data, size_t len,
                              sockaddr* addr, socklen_t* addr_len) {
  const size_t length = detail::SocketIoLength(len);
#ifdef ZNET_TARGET_WIN
  return recvfrom(socket, static_cast<char*>(data), static_cast<int>(length), 0,
                  addr, addr_len);
#else
  return recvfrom(socket, data, length, 0, addr, addr_len);
#endif
}

inline void SetTCPNoDelay(SocketHandle socket) {
  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline bool SetSocketBlocking(SocketHandle socket, bool blocking) {
#ifdef ZNET_TARGET_WIN
  u_long mode = blocking ? 0UL : 1UL; // 1 to enable non-blocking socket
  // FIONBIO carries IOC_IN so the macro is unsigned; the cmd param is signed.
  return ioctlsocket(socket, static_cast<long>(FIONBIO), &mode) == 0;
#else
  int flags = fcntl(socket, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |=  O_NONBLOCK;
  }
  return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

/**
 * @brief Waits until the socket will accept more data, or the timeout expires.
 *
 * poll rather than select: select's fd_set cannot hold a descriptor at or above
 * FD_SETSIZE, which a server with a few thousand connections will reach, and
 * FD_SET on one is undefined rather than an error.
 *
 * @return false only on a socket error. A timeout returns true, so the caller
 *         re-checks its own state and decides whether to keep waiting.
 */
inline bool WaitUntilWritable(SocketHandle socket, int timeout_ms) {
#ifdef ZNET_TARGET_WIN
  WSAPOLLFD fds{};
  fds.fd = socket;
  fds.events = POLLWRNORM;
  return WSAPoll(&fds, 1, timeout_ms) >= 0;
#else
  pollfd fds{};
  fds.fd = socket;
  fds.events = POLLOUT;
  int ready;
  do {
    ready = poll(&fds, 1, timeout_ms);
  } while (ready < 0 && errno == EINTR);  // a signal is not a send failure
  return ready >= 0;
#endif
}

}

#endif  // ZNET_UTIL_H_
