# znet benchmarks

Loopback benchmarks for znet's two transports, a raw-socket floor, and three
comparison libraries. Every library runs the same workloads through the same
reporting code (`common/harness.h`), so rows from different binaries line up
into one table.

**Results are in the [root README](../README.md#benchmarks).**

## Running

Benchmarks are off by default:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZNET_BUILD_BENCHMARKS=ON
cmake --build build --target benchmarks
./build/benchmarks/baseline-bench
./build/benchmarks/znet-bench
```

Comparison libraries are downloaded at configure time and enabled individually,
so one that stops building does not block the rest:

```sh
-DZNET_BENCH_ENET=ON      # small, clean CMake, builds anywhere
-DZNET_BENCH_RAKNET=ON    # archived upstream, needs -fpermissive (handled)
-DZNET_BENCH_GNS=ON       # Valve GameNetworkingSockets, needs protobuf + OpenSSL
```

GNS enables C and pulls a large dependency tree; configure it in a separate
build directory if you want to keep the normal one lean.

## Workloads

| case | payload | messages | what it exercises |
| --- | --- | --- | --- |
| 64B | 64 B | 50000 | per-message overhead; small enough that a datagram could hold many |
| 1KB | 1 KiB | 20000 | roughly one message per MTU-sized datagram |
| 8KB | 8 KiB | 5000 | forces fragmentation in a datagram transport |
| latency | 64 B | 2000 | ping-pong round trip, echoed by the peer |

Throughput counts messages the *receiver* actually delivered to the
application, not messages handed to the sender, so silent loss shows up as a
low count rather than a high rate.

**Payloads are incompressible by default** (`bench::MakePayload`). Filling a
buffer with one repeated byte, the obvious choice, flatters any library that
compresses: zstd takes 64 bytes of 'x' down to about 15, so several times as
many messages fit a datagram and the result describes the payload instead of the
transport. `PayloadKind` also offers `Snapshot` (game entity state) and `Text`,
because compression is worth wildly different amounts per traffic type and no
single payload is representative:

| payload | 64 B | 1 KiB | 8 KiB |
| --- | ---: | ---: | ---: |
| binary | 0.88x | 0.99x | 1.00x |
| snapshot | 0.88x | 1.41x | 1.51x |
| text | 0.88x | 2.93x | 4.08x |

Nothing compresses below roughly 96-128 bytes; the frame header makes every kind
larger. That is what `CommonOptions::compression_threshold` (default 128) is
for.

## Reading the numbers

These are not apples-to-apples, and the differences matter more than the
ranking. Before comparing any two rows, check which of these applies.

**znet encrypts and compresses every packet by default**, via the existing
`EncryptionLayer` plus zstd. ENet and RakNet here send plaintext and do not
compress; only GNS is comparable on that axis, encrypting with AES-GCM. So
`znet-bench` reports two profiles: `znet` as shipped, and `znet-raw` with both
off, which is the like-for-like row against ENet and RakNet.

The switch is an ordinary option, not benchmark-only scaffolding:

```cpp
ServerConfig config{"0.0.0.0", 25000};
config.child_options.common.encryption = false;
config.child_options.common.compression = CompressionType::None;
```

Both are **server-side only**. The server picks and announces its choice during
the handshake and the client adopts it, so clients need no matching config.
Setting either in `ClientConfig::options` does nothing.

**Compression runs before encryption**, so it applies to the plaintext and is
effective on encrypted and unencrypted sessions alike. It used to run after,
which compressed ciphertext: incompressible by construction, so the pass cost
time and saved nothing.

**Threading models differ, and this dominates the latency column.**

- ENet has no threads at all; the application drives it. The benchmark services
  both hosts in a tight spin loop, which is why its latency is essentially the
  raw UDP round trip. A real ENet application servicing at frame rate would see
  latency bounded by its frame interval instead. Its latency row is therefore
  not comparable with the polled libraries'.
- RakNet, GNS and znet own their threads and are polled. Their latency floors
  reflect internal update intervals, not the wire.
- znet's session workers tick at 120 Hz, but a backend with its own receive
  thread wakes them on arrival rather than letting them sleep out the tick, so a
  ZDT round trip is not bound to the tick interval. TCP reads on the worker
  itself and still is, which is why the two znet transports differ by three
  orders of magnitude there. `Server::SetTicksPerSecond()` changes the tick.

**Message rate is not byte rate.** A library that packs several small messages
into one datagram can report a message rate far above the raw UDP datagram
rate. That is real work, but it is measuring aggregation, not the socket. Check
the MiB/s column and the raw UDP floor together before concluding anything from
a msg/s number.

**The raw-socket baseline is a floor, not a rival.** It has no reliability,
ordering, encryption, or per-message allocation. It answers "what do the
syscalls alone cost on this machine".

## Known gaps

- RakNet's 8 KiB throughput case intermittently stalls at 4999 of 5000 delivered
  and runs into the 60 s deadline, reporting ~83 msg/s. Successful runs land
  near 62,000-67,000. Re-run before trusting that row.
- The 64 B throughput rows and the ZDT latency rows swing 20-50% between runs,
  for every library including ENet and GNS: each message costs little enough
  that scheduler noise dominates. Take the median of several runs, and do not
  read small differences there as real. The 1 KiB and 8 KiB columns are stable
  to a few percent.

- znet TCP cannot carry the 8KB case: `ZNET_MAX_BUFFER_SIZE` framing requires a
  whole message to fit in one buffer. The row reports `unsupported` rather than
  silently skipping.
- ZDT's congestion window is capped at 32 datagrams, because an acknowledgement
  describes one packet_seq plus 32 history bits and anything beyond that cannot
  be acknowledged at all. Widening `ack_bits`, or adding selective-ack ranges,
  is what would raise that ceiling; raising `max_datagrams_in_flight` on its own
  only produces datagrams the peer has no way to confirm.
