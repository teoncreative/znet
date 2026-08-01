# znet

znet is a modern C++ networking library for applications that send structured
messages: define a packet, register a serializer, send it. Encryption,
compression and framing are handled underneath. It is designed to be simpler and
more approachable than low-level libraries like asio or libuv, which give you a
socket and an event loop where znet gives you sessions, typed messages and an
event callback.

## Features

- ✅ **Simple API**: clean, event-driven design.
- 📦 **Built-in packet serialization**: define your own packets easily.
- 🔒 **Encryption and compression**: AES-256 and zstd, negotiated during the
  handshake. Read
  [what the crypto does and does not give you](https://github.com/teoncreative/znet/wiki/Encryption-and-Compression)
  before relying on it; it is not TLS and does not authenticate the peer.
- ⚡ **Async connect**: non-blocking connections.
- 🛠 **Cross-platform**: Windows, Linux, macOS.

## Installation

Requires **C++14 or newer** (C++20 by default), **CMake 3.29+** and **OpenSSL**,
which is found automatically if installed. zstd is bundled.

👉 **[Getting Started](https://github.com/teoncreative/znet/wiki/Getting-Started)**
has the submodule and CMake steps, using either the bundled zstd or your own,
plus every build option.

## What it looks like

**Server:**

```cpp
ServerConfig config{"127.0.0.1", 25000};
Server server{config};
server.SetEventCallback(...);
server.Bind();
server.Listen();  // async listen
```

**Client:**

```cpp
ClientConfig config{"127.0.0.1", 25000};
Client client{config};
client.SetEventCallback(...);
client.Bind();
client.Connect();  // async connect
```

Neither config names a transport, so both get the default, ZDT. See the
[examples](examples) folder for full working code, and the wiki below for
everything else.

## Documentation

👉 **[Read the Wiki](https://github.com/teoncreative/znet/wiki)**

* [Getting Started](https://github.com/teoncreative/znet/wiki/Getting-Started):
  build it, then a server and client that talk
* [Packets and Serialization](https://github.com/teoncreative/znet/wiki/Packets-and-Serialization):
  defining messages
* [Events](https://github.com/teoncreative/znet/wiki/Events): the six events and
  what to do in each
* [Choosing a Transport](https://github.com/teoncreative/znet/wiki/Choosing-a-Transport):
  ZDT or TCP, and ZDT's per-message delivery modes
* [Threading Model](https://github.com/teoncreative/znet/wiki/Threading-Model):
  which thread calls your code
* [Configuration Reference](https://github.com/teoncreative/znet/wiki/Configuration-Reference):
  every option, including metrics and the per-transport groups
* [Encryption and Compression](https://github.com/teoncreative/znet/wiki/Encryption-and-Compression):
  what the crypto does and does not give you
* [Metrics](https://github.com/teoncreative/znet/wiki/Metrics): the counters, and
  reading them honestly
* [Peer-to-Peer](https://github.com/teoncreative/znet/wiki/Peer-to-Peer):
  rendezvous and hole punching

## Benchmarks

Two workloads: a clean loopback, and the same traffic over a link with 5% packet
loss and a 50 ms round trip. **Methodology, reproduction, per-row caveats and
known gaps are in [benchmarks/README.md](benchmarks/README.md)**, which is where
these numbers are explained rather than here.

`znet` is the default build (AES-256 + zstd). `znet-raw` has both off, which is
the like-for-like row against ENet and RakNet, which send plaintext. Throughput
is messages per second at three payload sizes; latency is a 64 B ping-pong round
trip. Every row was measured in one sitting on one machine (Ryzen 9 9950X3D,
Linux 7.1.5, GCC), so they can be read against each other but not against a
table from anywhere else.

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

Read every row with its spread, not just its median. Five of the twelve
throughput cells vary by more than a quarter across their runs, and three by more
than half, so a single measurement from any of these libraries is not a number.

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

Footnotes, each explained in full in [benchmarks/README.md](benchmarks/README.md):
₁ znet ZDT collapses intermittently under loss, so these medians are honest but
not a steady state, and the cause is still open.
₂ Lower bound: GNS clamps its own send rate internally, so these rows measure
that limiter rather than the protocol.
₃ The same intermittent collapse as ZDT, measured on ENet and GNS rather than assumed
absent. Neither is steadier than znet under loss.
₄ Did not finish at the 60 s deadline.
₅ Stalls on the tail, so this cell is either ~74,000 or ~83 and never anything
between.

## Contributions

We welcome and encourage community contributions to improve znet. If you find any
bugs, have feature requests, or want to contribute in any other way, feel free to
open an issue or submit a pull request.

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.
