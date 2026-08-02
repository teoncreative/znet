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
- 🔒 **Encryption and compression**: AES-256-GCM and zstd, negotiated during the
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
* [Session State](https://github.com/teoncreative/znet/wiki/Session-State):
  attaching your own per-connection object to a session
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
* [Extensions](https://github.com/teoncreative/znet/wiki/Extensions): optional
  add-ons for smaller packets, automatic serialization and engine types
* [Peer-to-Peer](https://github.com/teoncreative/znet/wiki/Peer-to-Peer):
  rendezvous and hole punching

## Benchmarks

Two link conditions, a clean loopback and the same traffic over a link with 5%
packet loss and a 50 ms round trip, measured three ways: peak throughput, idle
round trip, and a sustained transfer with a probe running through it.

Below are the comparison tables and a short read of each.
**[benchmarks/README.md](benchmarks/README.md) covers how they were produced:**
running and reproducing them, what each measurement does and does not support,
the per-row caveats behind the footnotes, and the open problems. It also carries
the measurements that are not headline comparisons, such as what compression is
worth per traffic type.

`znet` is the default build (AES-256-GCM + zstd). `znet-raw` has both off, which is
the like-for-like row against ENet and RakNet, which send plaintext. Throughput
is messages per second at three payload sizes; latency is a 64 B ping-pong round
trip. Every row was measured in one sitting on one machine (Ryzen 9 9950X3D,
Linux 7.1.5, GCC), so they can be read against each other but not against a
table from anywhere else.

### Over a lossy link

5% packet loss, 50 ms round trip. Message counts are scaled down for the longer
round trip, so these numbers compare only with each other.

Each cell is the median of five runs; latency percentiles pool all five.

|                             |       64 B |     1 KiB |         8 KiB |         p50 |     p95 |     p99 |
|-----------------------------|-----------:|----------:|--------------:|------------:|--------:|--------:|
| **znet ZDT** ₁              | **27,708** |     5,374 |           894 |     50.1 ms |     151 |     210 |
| **znet ZDT-raw** ₁          |     25,395 | **6,714** |     **1,033** |     50.1 ms |     151 |     201 |
| GNS (Valve) ₂ ₃             |     18,018 |     1,619 |            34 |     50.1 ms |     260 |     262 |
| ENet ₃                      |     14,077 |     1,027 |           131 | **50.0 ms** | **112** | **137** |
| raw UDP *(syscall floor)* ₅ |     38,603 |    38,804 |         5,098 |     50.1 ms |    50.1 |    50.2 |
| raw TCP *(syscall floor)*   |      2,865 |       133 |            19 |     50.1 ms |     402 |     471 |
| **znet TCP**                |      1,635 |     129 ₄ | *unsupported* |     50.4 ms |     310 |     434 |
| **znet TCP-raw**            |      1,619 |     136 ₄ | *unsupported* |     50.4 ms |     311 |     462 |
| RakNet                      |      1,070 |      69 ₄ |           9 ₄ |     70.1 ms |     230 |     261 |

znet leads every payload size here: ZDT at 64 B and the raw arm at 1 KiB and
8 KiB, the two being inside each other's spread. Taking the default ZDT row,
that is 1.5x, 3.3x and 26x what GNS does and 2.0x, 5.2x and 6.8x what ENet
does. On the tail the two encrypted transports split: znet holds p95 at 151 ms
against GNS's 260, and p99 at 210 against 262. Of the reliable transports ENet
is the tightest at every
percentile, at a fifth to a seventh of znet ZDT's throughput at 1 KiB and above;
the raw UDP row is lower still but retransmits nothing, so it is a floor rather
than a rival.

Read every row with its spread, not just its median. Seventeen of the
twenty-eight throughput cells measured vary by more than a quarter across their
five runs and seven by more than double, against one and none respectively on
the clean table, so a single measurement under loss is not a number.
`ZNET_BENCH_REPS` is what produced the medians here.

### Clean loopback

|                           |          64 B |       1 KiB |         8 KiB | encryption  |
|---------------------------|--------------:|------------:|--------------:|-------------|
| ENet                      |     5,742,000 |   1,191,000 |       170,000 | none        |
| **znet ZDT-raw**          | **1,978,000** | **333,000** |        59,900 | none        |
| **znet ZDT**              | **1,679,000** | **306,000** |    **60,500** | AES-256-GCM |
| raw UDP *(syscall floor)* |     1,647,000 |   1,579,000 |     1,043,000 | n/a         |
| GNS (Valve) ₂             |     1,459,000 |      98,800 |        11,500 | AES-GCM     |
| raw TCP *(syscall floor)* |       614,000 |     547,000 |       488,000 | n/a         |
| RakNet                    |        48,200 |      42,200 |        76,800 | none        |
| **znet TCP-raw**          |    **30,400** |  **30,200** | *unsupported* | none        |
| **znet TCP**              |    **30,400** |  **30,200** | *unsupported* | AES-256-GCM |

|                   |          p50 |      p95 |      p99 |
|-------------------|-------------:|---------:|---------:|
| raw UDP *(floor)* |       3.1 µs |      5.5 |      7.1 |
| ENet              |       3.3 µs |      3.4 |      3.4 |
| raw TCP *(floor)* |       4.5 µs |      6.0 |      7.4 |
| GNS (Valve)       |      10.0 µs |    1,063 |    2,115 |
| **znet ZDT-raw**  |  **12.5 µs** | **14.8** | **17.9** |
| **znet ZDT**      |  **13.5 µs** | **15.2** | **19.5** |
| **znet TCP-raw**  | **8,387 µs** |    8,400 |    8,404 |
| **znet TCP**      | **8,392 µs** |    8,401 |    8,407 |
| RakNet            |    20,067 µs |   20,088 |   20,120 |

ENet leads every library here on both. Of the two encrypted datagram transports,
znet ZDT holds p95 within 1.2x of its median and p99 within 1.5x, where GNS sits
106x and 211x above its own. The default profile costs about 1.18x at 64 B and
1.09x at 1 KiB against `znet-raw`, and is inside the run-to-run spread at
8 KiB, so it is per-message overhead rather than per-byte cost. That gap is
crypto *and* compression: `-raw` turns both off, and above the 128 B threshold
the default profile also pays a zstd pass that incompressible payloads cannot
repay.

Loopback has no loss and a microsecond round trip, so neither congestion control
nor loss recovery runs; see
[why the clean tables flatter TCP](benchmarks/README.md#why-the-clean-tables-flatter-tcp).

### Under sustained load

The tables above measure a burst finishing as fast as it can. This one runs a
1 KiB transfer for ten seconds and sends a 64 B probe through it, so `steady` is
the rate over the second half, once slow-start is past, and `loaded` is what a
small message costs while the link is saturated. That pair is what a congestion
controller is actually judged on, and it is the only view here where a
transport can win the throughput column by simply filling a queue and leaving
everything else stuck behind it.

The `probe` column says how the probe was kept off the bulk stream, and the
loaded figures only compare like for like within one value of it: `channel` is
its own ordered stream, `conn` its own connection, `none` the same stream as the
transfer, where it is head-of-line blocked as well as queued.

|                         | probe   |  clean steady | clean loaded p50 | lossy steady | lossy loaded p50 |
|-------------------------|---------|--------------:|-----------------:|-------------:|-----------------:|
| ENet                    | channel | **1,157,000** |           3.6 ms |        1,043 |         1,864 ms |
| raw TCP *(cubic floor)* | conn    |       564,000 |      **0.03 ms** |          163 |        **50 ms** |
| **znet ZDT-raw**        | channel |   **370,000** |          11.0 ms |    **5,591** |           481 ms |
| **znet ZDT**            | channel |   **319,000** |          12.5 ms |    **4,867** |           633 ms |
| GNS (Valve)             | none    |        86,200 |          47.4 ms |        1,481 |                ₆ |
| RakNet                  | channel |        47,600 |          87.6 ms |           69 |                ₆ |
| **znet TCP**            | none    |    **30,500** |         140.0 ms |          123 |                ₆ |

Under loss ZDT carries 3.3x what GNS does and 4.7x what ENet does. Against ENet,
the one row measured the same way, it does that while holding a small message to
a third of the delay: that is the trade a delay-sensitive controller exists to
make, and it is the clearest thing the suite shows.

On clean loopback the same rows are less flattering. ZDT's loaded p50 is 12.5 ms
against ENet's 3.6 ms on the same separation, and against 0.03 ms for kernel TCP
on its own connection. Saturated, a small message waits behind the bulk queue
even on its own channel, because a separate channel buys a separate sequence
space rather than a separate queue. Unexplained, and the first thing to look at
if the send path is reworked.

Footnotes, each explained in full in [benchmarks/README.md](benchmarks/README.md):
₁ znet ZDT collapses intermittently under loss, so these medians are honest but
not a steady state, and the cause is still open. Every ZDT cell spans 2.8x to
4.3x across its five runs, the worst being 8 KiB at 351..1,497 msg/s.
₂ Lower bound: GNS clamps its own send rate internally, so these rows measure
that limiter rather than the protocol.
₃ The same intermittent collapse as ZDT, measured on ENet and GNS rather than
assumed absent. Neither is steady under loss either: GNS's 1 KiB row spans 2.7x
and ENet's 64 B row 1.5x, though ENet is the tighter of the two here.
₄ Did not finish at the 60 s deadline; the row is marked `TIMEOUT` in the
benchmark's own output.
₅ Unreliable, so it is not a rival: at 5% loss it simply drops what it loses and
its counts are what arrived, not what a reliable protocol had to recover.
₆ Too few probes returned inside the 2 s timeout to quote a percentile: none at
all for RakNet and znet TCP, and two of six for GNS. The link being too
congested for a small message to complete a round trip is itself the result.

## Contributions

We welcome and encourage community contributions to improve znet. If you find any
bugs, have feature requests, or want to contribute in any other way, feel free to
open an issue or submit a pull request.

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.
