# znet

znet is a modern C++20 networking library that provides seamless packet serialization, TLS encryption, and cross-platform support. It's designed to be simpler and more approachable than low-level libraries like asio or libuv.

## Features

- ✅ **Simple API** – Clean, event-driven design.
- 🔒 **TLS Encryption** – Secure communication out of the box.
- ⚡ **Async Connect** – Non-blocking connections.
- 📦 **Built-in Packet Serialization** – Define your own packets easily.
- 🛠 **Cross-Platform** – Windows, Linux, macOS.

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

* **C++20** (GCC 10+, Clang 13+, MSVC 19.29+)
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
````

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
config.child_options.zdt.cwnd = 128;
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

Loopback, single machine, Release build, median of 3 runs. `znet` is the default
configuration (AES + zstd), `znet-raw` has both disabled. Methodology and
reproduction steps: [benchmarks/README.md](benchmarks/README.md).

Throughput, messages per second:

| | 64 B | 1 KiB | 8 KiB | encryption |
| --- | ---: | ---: | ---: | --- |
| raw UDP *(syscall floor)* | 1,159,000 | 1,192,000 | 822,000 | — |
| raw TCP *(syscall floor)* | 431,000 | 402,000 | 381,000 | — |
| ENet | 4,979,000 | 924,000 | 115,000 | none |
| GNS (Valve) | 1,023,000 | 81,300 | 9,300 | AES-GCM |
| **znet ZDT** | **546,000** | **70,900** | **10,800** | AES-256 |
| **znet ZDT-raw** | **531,000** | **81,000** | **10,300** | none |
| RakNet | 48,000 | 41,800 | 63,600 † | none |
| **znet TCP** | **30,300** | **25,800** | *unsupported* | AES-256 |
| **znet TCP-raw** | **30,400** | **27,400** | *unsupported* | none |

Round-trip latency, 64 B ping-pong:

| | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| raw UDP *(floor)* | 3.2 µs | 4.3 | 11.9 |
| ENet | 4.0 µs | 4.0 | 4.1 |
| raw TCP *(floor)* | 6.4 µs | 8.8 | 12.1 |
| **znet ZDT** | **22.1 µs** | 27.9 | 43.0 |
| **znet ZDT-raw** | **26.4 µs** | 36.7 | 46.9 |
| **znet TCP** | **8,386 µs** | 8,401 | 8,412 |
| **znet TCP-raw** | **8,386 µs** | 8,395 | 8,402 |
| GNS (Valve) | 10,151 µs | 11,215 | 12,263 |
| RakNet | 20,053 µs | 20,118 | 20,157 |

The 64 B throughput and ZDT latency figures swing 20-50% between runs for every
library here, so treat them as an order of magnitude. The 1 KiB and 8 KiB
columns and the polled libraries' latency are stable to a few percent. `znet` and
`znet-raw` sit inside each other's spread at 64 B, so this workload cannot
resolve what encryption costs there.

† RakNet's 8 KiB case intermittently stalls; see
[known gaps](benchmarks/README.md#known-gaps).

### Reading these rows

Enough to avoid misreading the tables; the reasoning behind each point, the
workload definitions and the per-library quirks are in
[benchmarks/README.md](benchmarks/README.md#reading-the-numbers).

- **Loopback only.** No link, no loss, no propagation delay. Relative, not
  absolute.
- **ENet's latency is not comparable.** It has no threads and is serviced in a
  spin loop here; RakNet, GNS and znet own theirs and are polled.
- **znet TCP's latency is one 120 Hz server tick**, because TCP is read on the
  session worker. ZDT has its own receive thread and wakes the worker on
  arrival, which is the whole difference between those rows.
- **Some rows beat the raw UDP floor** because ENet and ZDT pack several messages
  into one datagram, so a message rate is not a datagram rate. The benchmarks
  print bytes per second alongside, which is the figure to check.
- **Compression is off the table here on purpose.** Payloads are incompressible,
  so these columns describe transports. What compression is actually worth by
  traffic type is measured in
  [benchmarks/README.md](benchmarks/README.md#workloads).

## Documentation

More details:

* **Usage guides**
* **TLS configuration**
* **Serialization**

👉 [Read the Wiki](https://github.com/irrld/znet/wiki)

## Contributions

We welcome and encourage community contributions to improve znet. If you find any bugs, have feature requests, or want to contribute in any other way, feel free to open an issue or submit a pull request.

## License

Apache License 2.0 – see [LICENSE](LICENSE) for details.
