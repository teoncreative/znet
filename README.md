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
git submodule add https://github.com/irrld/znet.git external/znet
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
trip. Medians of 7 runs, 5 for the comparison libraries under loss, 3 for
RakNet.

### Over a lossy link

5% packet loss, 50 ms round trip. Message counts are scaled down for the longer
round trip, so these numbers compare only with each other.

| | 64 B | 1 KiB | 8 KiB | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **znet ZDT-raw** | **22,500** | **6,850** | **1,110** | 50.0 ms | 151 | 230 |
| **znet ZDT** | 21,200 | 6,510 | 1,040 | 50.1 ms | 150 | 173 |
| GNS (Valve) ‡ | 19,100 | 3,430 | 374 | 50.1 ms | 260 | 262 |
| ENet | 8,340 | 975 | 130 | 50.0 ms | **112** | **112** |
| **znet TCP** | 1,470 | 149 | *unsupported* | 50.3 ms | 311 | 399 |
| RakNet | 1,050 | 73 ¶ | 9 ¶ | 70.1 ms | 230 | 381 |

znet ZDT carries 1.1x, 1.9x and 2.8x what GNS does at 64 B, 1 KiB and 8 KiB,
with a p99 1.5x tighter, and 2.5x, 6.7x and 8.0x what ENet does. ENet keeps the
tightest tail.

### Clean loopback

| | 64 B | 1 KiB | 8 KiB | encryption |
| --- | ---: | ---: | ---: | --- |
| ENet | 5,612,000 | 1,131,000 | 147,000 | none |
| **znet ZDT-raw** | **2,073,000** | **309,000** | **67,600** | none |
| **znet ZDT** | **1,505,000** | **175,000** | **41,800** | AES-256 |
| raw UDP *(syscall floor)* § | 1,499,000 | 906,000 | 663,000 | n/a |
| GNS (Valve) ‡ | 1,027,000 | 98,800 | 11,700 | AES-GCM |
| raw TCP *(syscall floor)* § | 549,000 | 273,000 | 207,000 | n/a |
| RakNet | 48,200 | 41,600 | 69,200 † | none |
| **znet TCP-raw** | **30,400** | **23,400** | *unsupported* | none |
| **znet TCP** | **30,300** | **28,400** | *unsupported* | AES-256 |

| | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| ENet | 3.3 µs | 3.3 | 3.4 |
| raw TCP *(floor)* | 5.0 µs | 10.3 | 11.1 |
| raw UDP *(floor)* | 6.0 µs | 7.0 | 7.8 |
| GNS (Valve) | 10.0 µs | 1,066 | 2,116 |
| **znet ZDT-raw** | **11.8 µs** | **14.5** | **18.3** |
| **znet ZDT** | **12.5 µs** | **14.8** | **16.4** |
| **znet TCP-raw** | **8,386 µs** | 8,399 | 8,410 |
| **znet TCP** | **8,386 µs** | 8,400 | 8,412 |
| RakNet | 20,059 µs | 20,084 | 20,115 |

ENet leads loopback on both. Of the two encrypted datagram transports, znet ZDT
holds p95 and p99 within 1.3x of its median where GNS sits 107x and 212x above
its own. Encryption costs 1.6-1.8x at 1 KiB and 8 KiB.

Loopback has no loss and a microsecond round trip, so neither congestion control
nor loss recovery runs; see
[why the clean tables flatter TCP](benchmarks/README.md#why-the-clean-tables-flatter-tcp).

‡ Send-rate limited, so these rows are a lower bound.
† Intermittently stalls; swings between roughly 70,000 and double digits.
§ Bimodal as measured; the reproduction steps now pin inside one L3 domain.
¶ Did not finish: 4,352 of 8,000 and 530 of 2,000 at the 60 s deadline.

## Documentation

More details:

* **Usage guides**
* **TLS configuration**
* **Serialization**

👉 [Read the Wiki](https://github.com/irrld/znet/wiki)

## Contributions

We welcome and encourage community contributions to improve znet. If you find any bugs, have feature requests, or want to contribute in any other way, feel free to open an issue or submit a pull request.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.
