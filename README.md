# znet

znet is a modern C++ networking library that provides seamless packet serialization, TLS encryption, and cross-platform support. It's designed to be simpler and more approachable than low-level libraries like asio or libuv.

## Features

- ✅ **Simple API**: Clean, event-driven design.
- 🔒 **TLS Encryption**: Secure communication out of the box.
- ⚡ **Async Connect**: Non-blocking connections.
- 📦 **Built-in Packet Serialization**: Define your own packets easily.
- 🛠 **Cross-Platform**: Windows, Linux, macOS.

## Installation

### Using as a Git Submodule

1. **Add znet to your project:**

```bash
git submodule add https://github.com/teoncreative/znet.git external/znet
git submodule update --init --recursive
```

2. **Link znet in your `CMakeLists.txt`:**

Example using the bundled `zstd` inside znet

```cmake
# Example using the bundled zstd inside znet
add_subdirectory(external/znet/vendor/zstd/build/cmake ${CMAKE_CURRENT_BINARY_DIR}/zstd)
add_subdirectory(external/znet/znet ${CMAKE_CURRENT_BINARY_DIR}/znet)
target_link_libraries(your_target PRIVATE znet)
```

Example using your own `zstd` submodules

```cmake
# Example using your own submodules
add_subdirectory(external/zstd/build/cmake)
add_subdirectory(external/znet/znet)
target_link_libraries(your_target PRIVATE znet)
```

Example using system-installed `zstd` (e.g. vcpkg, brew, etc.)

```cmake
# Example using system-installed zstd (e.g. vcpkg, brew, etc.)
set(ZNET_USE_EXTERNAL_ZSTD ON)
add_subdirectory(external/znet/znet)
target_link_libraries(your_target PRIVATE znet)
```

3. **Requirements:**

* **C++14 or newer.** The default is C++20; set `-DZNET_CXX_STANDARD=14`, `17`,
  `20` or `23` to pick one. Raising it only ever adds capability; the API is the same either way.
* **CMake 3.29+**
* **OpenSSL**
  * Install via package manager (e.g. `libssl-dev` on Linux, `vcpkg` on Windows, `brew` on macOS)
  * znet will automatically detect and link OpenSSL if it's installed

## Quick Example

Below is a minimal overview of how to use znet.

**Server:**
```cpp
ServerConfig config{"127.0.0.1", 25000};
Server server{config};
server.SetEventCallback(...);
server.Bind();
server.Listen(); // Async listen
```

**Client:**

```cpp
ClientConfig config{"127.0.0.1", 25000};
Client client{config};
client.SetEventCallback(...);
client.Bind();
client.Connect(); // Async connect
```

**Options:**
Options are scoped like Netty's: `options` configure the thing you created,
`child_options` configure each session a server accepts.

```cpp
ServerConfig config{"0.0.0.0", 25000, std::chrono::seconds(10),
                    ConnectionType::ZDT};      // reliable UDP instead of TCP
config.options.max_connections = 4096;         // the listener
config.child_options.tcp.no_delay = true;      // every accepted session
config.child_options.zdt.max_datagrams_in_flight = 256;
```

**Metrics:**
Counters are pull-based, so sample them on a timer rather than per packet.
Build with `-DZNET_ENABLE_METRICS=OFF` to compile them out entirely.

Counters are grouped like options are: what every transport has lives in
`common`, and each transport gets its own group. `transport` says which group is
populated.

```cpp
SessionMetrics m = session->metrics();
m.common.messages_sent;   // any transport
m.tcp.writes;             // TCP only
m.zdt.retransmits;        // ZDT only

ServerMetrics s = server.metrics();
s.connections_accepted;
s.zdt.cookies_rejected;   // ZDT only
```

**Packets:**
Implement `Packet` and `PacketSerializer` to define your messages.

See the [examples](examples) folder for full working code.

## Benchmarks

Two workloads: a clean loopback, and the same traffic over a link with 5% packet
loss and a 50 ms round trip. Methodology, reproduction and caveats:
[benchmarks/README.md](benchmarks/README.md).

`znet` is the default build (AES-256 + zstd). `znet-raw` has both off, which is
the like-for-like row against ENet and RakNet, which send plaintext. Throughput
is messages per second at three payload sizes; latency is a 64 B ping-pong round
trip.

Every row in every table below was measured in one sitting on one machine, so
they can be read against each other. Ryzen 9 9950X3D, Linux 7.1.5, GCC, all
libraries built from source at the versions
[benchmarks/external](benchmarks/external/CMakeLists.txt) pins. Medians of 15
runs for znet clean and 7 impaired, 7 and 5 for the comparison libraries, 3 for
RakNet either way. Rebuild the whole set rather than editing single rows: the
numbers move enough between machines and kernels that a table mixing two of them
says nothing.

### Over a lossy link

5% packet loss, 50 ms round trip. Message counts are scaled down for the longer
round trip, so these numbers compare only with each other.

| | 64 B | 1 KiB | 8 KiB | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **znet ZDT-raw** ₁ | **22,561** | **6,745** | 1,032 | **50.1 ms** | 151 | 218 |
| **znet ZDT** ₁ | 21,287 | 6,218 | **1,260** | 50.1 ms | 151 | 251 |
| GNS (Valve) ₂ ₃ | 17,713 | 853 | 121 | 50.1 ms | 261 | 262 |
| ENet ₃ | 8,649 | 896 | 122 | **50.0 ms** | **112** | **124** |
| **znet TCP-raw** | 1,860 | 136 ₄ | *unsupported* | 50.3 ms | 311 | 390 |
| **znet TCP** | 1,453 | 137 ₄ | *unsupported* | 50.3 ms | 310 | 412 |
| RakNet | 1,117 | 71 ₄ | 8 ₄ | 70.1 ms | 230 | 340 |

znet ZDT leads every payload size here, carrying 1.2x, 7.3x and 10.4x what GNS
does and 2.5x, 6.9x and 10.3x what ENet does. On the tail the two encrypted
transports split: znet holds p95 at 151 ms against GNS's 261, and p99 at 251
against 262. ENet is the tightest at every percentile, at a tenth of znet ZDT's
throughput at 1 KiB and above.

Read every row here with its spread, not just its median. Five of the twelve
throughput cells vary by more than a quarter across their runs, and three vary
by more than half, so a single measurement from any library is not a number.

### Clean loopback

| | 64 B | 1 KiB | 8 KiB | encryption |
| --- | ---: | ---: | ---: | --- |
| ENet | 5,444,000 | 1,116,000 | 149,000 | none |
| **znet ZDT-raw** | **1,910,000** | **297,000** | **55,000** | none |
| raw UDP *(syscall floor)* | 1,627,000 | 1,571,000 | 966,000 | n/a |
| **znet ZDT** | **1,493,000** | **288,000** | **48,700** | AES-256 |
| GNS (Valve) ₂ | 1,484,000 | 98,800 | 11,600 | AES-GCM |
| raw TCP *(syscall floor)* | 573,000 | 541,000 | 484,000 | n/a |
| RakNet | 48,600 | 42,600 | 74,000 ₅ | none |
| **znet TCP-raw** | **30,400** | **29,100** | *unsupported* | none |
| **znet TCP** | **30,300** | **29,800** | *unsupported* | AES-256 |

| | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| raw UDP *(floor)* | 3.1 µs | 4.3 | 5.2 |
| ENet | 3.3 µs | 3.3 | 3.4 |
| raw TCP *(floor)* | 4.5 µs | 5.9 | 7.1 |
| GNS (Valve) | 10.7 µs | 1,072 | 2,129 |
| **znet ZDT-raw** | **13.3 µs** | **15.2** | **18.8** |
| **znet ZDT** | **14.6 µs** | **16.8** | **23.7** |
| **znet TCP-raw** | **8,386 µs** | 8,397 | 8,402 |
| **znet TCP** | **8,386 µs** | 8,398 | 8,410 |
| RakNet | 20,039 µs | 20,076 | 20,100 |

ENet leads every library here on both. Of the two encrypted datagram transports,
znet ZDT holds p95 within 1.2x of its median and p99 within 1.6x, where GNS sits
100x and 199x above its own. Encryption costs about 1.1x at 8 KiB.

Loopback has no loss and a microsecond round trip, so neither congestion control
nor loss recovery runs; see
[why the clean tables flatter TCP](benchmarks/README.md#why-the-clean-tables-flatter-tcp).
It also flatters the raw-socket floors, which is why raw UDP outruns every
library at 1 KiB and 8 KiB: it is one `sendto` per message with no sequencing,
no acknowledgement and no reassembly underneath it.

₁ Collapse mode under loss: individual measurements fall to a fraction of the
usual rate, independently of each other, so a run can be fast at 64 B and
collapsed at 8 KiB. znet ZDT hit it in 3 of 7 runs at 1 KiB (~1,150 against
~6,200) and 1 of 7 at both 64 B and 8 KiB; the raw arm in 2 of 7 at 8 KiB. The
medians above are the honest summary of what a lossy link delivers, but they are
not a steady state, and the cause is still open.
₂ Lower bound: GNS clamps its own send rate to 100 MiB/s internally, whatever
SendRateMin/SendRateMax are set to, so these rows measure that limiter rather
than the protocol. See benchmarks/README.md.
₃ Same caveat as 1, measured on the comparison libraries rather than assumed
absent: ENet spans 2,428 to 13,174 across five runs at 64 B, and GNS 62 to 236
at 8 KiB. Neither is steadier than znet under loss.
₄ Did not finish at the 60 s deadline. RakNet: ~4,200 of 8,000 at 1 KiB, ~510 of
2,000 at 8 KiB, in every run. znet TCP: 2 of 7 runs at 1 KiB, lowest 7,575 of
8,000; TCP-raw 3 of 7, lowest 7,866.
₅ Stalls on the tail. One run of three delivered 4,999 of 5,000 messages at full
speed and then hung on the last one until the 60 s deadline, reporting 83 msg/s;
the other two finished in 0.067 s. The median above is one of those two, so this
cell is either ~74,000 or ~83 and never anything between.

## Documentation

👉 [Read the Wiki](https://github.com/teoncreative/znet/wiki)

* [Getting Started](https://github.com/teoncreative/znet/wiki/Getting-Started) — build it, then a server and client that talk
* [Packets and Serialization](https://github.com/teoncreative/znet/wiki/Packets-and-Serialization) — defining messages
* [Events](https://github.com/teoncreative/znet/wiki/Events) — the six events and what to do in each
* [Choosing a Transport](https://github.com/teoncreative/znet/wiki/Choosing-a-Transport) — TCP or ZDT, and ZDT's delivery modes
* [Threading Model](https://github.com/teoncreative/znet/wiki/Threading-Model) — which thread calls your code
* [Configuration Reference](https://github.com/teoncreative/znet/wiki/Configuration-Reference) — every option
* [Encryption and Compression](https://github.com/teoncreative/znet/wiki/Encryption-and-Compression) — what the crypto does and does not give you
* [Metrics](https://github.com/teoncreative/znet/wiki/Metrics) — the counters, and reading them honestly
* [Peer-to-Peer](https://github.com/teoncreative/znet/wiki/Peer-to-Peer) — rendezvous and hole punching

## Contributions

We welcome and encourage community contributions to improve znet. If you find any bugs, have feature requests, or want to contribute in any other way, feel free to open an issue or submit a pull request.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.
