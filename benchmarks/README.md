# znet benchmarks

Benchmarks for znet's two transports, a raw-socket floor and three comparison
libraries, on clean loopback and under netem impairment. Every library runs the same workloads through the same
reporting code (`common/harness.h`), so rows from different binaries line up
into one table.

**Results are in the [root README](../README.md#benchmarks).**

## Running

Benchmarks are off by default:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZNET_BUILD_BENCHMARKS=ON
cmake --build build --target benchmarks

# in a namespace of its own, so the traffic stays off the host's loopback
unshare -rn sh -c 'ip link set lo up
  taskset -c 0-7,16-23 ./build/benchmarks/znet-bench'
```

Use a namespace even when not impairing anything: these saturate loopback and
znet starts `hardware_concurrency()` worker threads, which otherwise competes
with anything of yours on localhost. A fresh namespace's `lo` is the same 65536
MTU, so the numbers are unaffected.

Pick the core list to sit inside one L3 domain. On a multi-CCD part a set that
straddles the boundary lets client and server land on different dies from run
to run, and the extra hop shows up as a bimodal result rather than as noise:
the raw socket baselines split into two clusters about 2x apart that way. Check
yours with

```sh
cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list
```

and use a list from a single group. The `0-7,16-23` above is one domain of a
Ryzen 9 9950X3D, 8 physical cores and their SMT siblings.

Comparison libraries are downloaded at configure time and enabled individually,
so one that stops building does not block the rest:

```sh
-DZNET_BENCH_ENET=ON      # small, clean CMake, builds anywhere
-DZNET_BENCH_RAKNET=ON    # archived upstream, needs -fpermissive (handled)
-DZNET_BENCH_GNS=ON       # Valve GameNetworkingSockets, needs protobuf + OpenSSL
```

GNS enables C and pulls a large dependency tree; configure it in a separate
build directory if you want to keep the normal one lean.

## Impaired runs

To reach the regimes that separate these protocols, put netem on the loopback
of an unprivileged network namespace and tell the benchmark what you did:

```sh
unshare -rn sh -c '
  ip link set lo mtu 1500
  ip link set lo up
  tc qdisc add dev lo root netem delay 25ms loss 5%
  ZNET_BENCH_IMPAIR="delay=25,loss=5" \
    taskset -c 0-7,16-23 ./build/benchmarks/znet-bench'
```

No root, and the host's networking is untouched. `delay` and `jitter` are one
way, so a round trip pays each twice.

The environment variable does not impair anything - netem does. It exists
because nothing can read netem's settings back, and the benchmark needs them
for two things: scaling the workloads (the stock 2,000 ping-pongs is ten
minutes at a 300 ms round trip) and labelling the output. Give it the same
numbers you gave netem or the counts will be wrong.

netem rather than a forwarder inside the benchmark, because it sits below the
socket and so impairs TCP too. A userspace forwarder can only corrupt a proxied
TCP stream, not drop from it, and would leave the TCP rows quietly unimpaired.

### Why the clean tables flatter TCP

Loopback's MTU is 65536 and the kernel does not really segment on it: a TCP
write there is a copy between socket buffers, with no reliability bookkeeping in
userspace and no syscall per datagram. Dropping the MTU to 1500 moves raw TCP by
about 2% at 8 KiB but halves raw UDP, which then pays IP fragmentation while TCP
still does not.

So the raw TCP row is a floor, not a rival, and its 8 KiB and latency figures
describe a memcpy rather than a network.

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

**Payloads are incompressible by default** (`bench::MakePayload`): one repeated
byte flatters anything that compresses, since zstd takes 64 bytes of 'x' down to
about 15. `PayloadKind` also offers `Snapshot` (game entity state) and `Text`,
because compression is worth wildly different amounts per traffic type:

| payload | 64 B | 1 KiB | 8 KiB |
| --- | ---: | ---: | ---: |
| binary | 0.88x | 0.99x | 1.00x |
| snapshot | 0.88x | 1.41x | 1.51x |
| text | 0.88x | 2.93x | 4.08x |

Nothing compresses below roughly 96-128 bytes; the frame header makes every kind
larger. That is what `CommonOptions::compression_threshold` (default 128) is
for.

Payloads in the comparison tables are incompressible, so those columns describe
transports rather than compression. What compression is worth by traffic type is
the ratio table above.

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
effective on encrypted and unencrypted sessions alike.

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

**GNS is rate-limited by its own design, and its rows are a lower bound.** Its
byte rate barely moves with payload size, 88, 96 and 91 MiB/s at 64 B, 1 KiB and
8 KiB, which is what a send-rate cap looks like rather than what protocol cost
looks like. znet's rows over the same three sizes go 82, 194 and 333 MB/s,
scaling the way amortised per-message overhead should.

The cap is not a configuration mistake. GNS clamps its send rate internally, in
`steamnetworkingsockets_snp.cpp`:

```cpp
int nMin = Clamp( m_connectionConfig.SendRateMin.Get(), 1024, 100*1024*1024 );
int nMax = Clamp( m_connectionConfig.SendRateMax.Get(), nMin, 100*1024*1024 );
```

100 MiB/s, whatever `SendRateMin`/`SendRateMax` are set to, and the config
itself refuses anything above `0x10000000`. Sweeping the setting with
`GNS_SEND_RATE` shows throughput tracking it linearly at about 96.5% up to the
clamp and flat above it: 25 MiB/s gives 24.1, 50 gives 48.3, and both 100 and
256 give 96.5. The missing 3.5% is header overhead. So the 1 KiB and 8 KiB
comparisons measure the limiter, not the protocol, and no setting available to a
caller changes that.

`gns_bench.cc` raises everything that *is* reachable: the ~256 KB/s default send
rate, `SendBufferSize` from its 512 KB default, and the receive-side
`RecvBufferSize` and `RecvBufferMessages`. The receive limits matter most at
64 B, where the 1024-message default binds and costs about 20%; both drop
packets rather than applying backpressure when exceeded, so leaving them at the
defaults would have measured GNS's anti-flood protection.

Its latency tail is its own, though, not an artifact of the setup. The
benchmark sets `NagleTime=0` so GNS sends immediately, as ZDT does; left at its
5 ms default it reports a ~10 ms round trip that measures Nagle rather than the
protocol.

It is also not doing the same work. GNS estimates bandwidth and paces its
sends where ZDT's controller only reads queueing delay, and GNS carries a
reliable byte stream rather than discrete messages, with NAT traversal, relay
fallback and certificate auth that nothing here exercises.

## Known gaps

- RakNet's 8 KiB throughput case intermittently stalls at 4999 of 5000 delivered
  and runs into the 60 s deadline, reporting ~83 msg/s. Successful runs land
  near 74,000. One run in three stalled for the published table, so re-run
  before trusting that row.
- **znet ZDT collapses intermittently under loss.** This is now the largest
  caveat in the table. At 5% loss and a 50 ms round trip, single measurements
  drop to a fraction of the usual rate: 1 KiB fell to ~1,150 msg/s against a
  healthy ~6,200 in three runs of seven, and 64 B and 8 KiB once each. The raw
  arm hit it twice of seven at 8 KiB.
  It is per-measurement, not per-run: a run is routinely fast at one payload
  and collapsed at the next, which rules out the machine being busy for the
  duration. It is not the impairment being uneven either, since a collapsed and
  a healthy measurement sit in the same run under the same qdisc. Note that GNS
  and ENet are not steadier here: across the same runs ENet spanned 5.4x at 64 B
  and GNS 3.8x at 8 KiB, so this is a property of running over a lossy link,
  not something peculiar to ZDT. Unexplained; treat the impaired medians of
  every library as an average over regimes rather than a steady rate.
- The 64 B column needs several runs to mean anything. Scheduler noise dominates
  when a message costs this little. On the clean table the fifteen runs behind
  the znet ZDT row spanned 1.07x, but under impairment the same column spanned
  4.6x for znet and 5.4x for ENet. A single run there is not a number.
- The raw TCP and raw UDP floors span 1.03-1.19x across seven runs with the core
  list pinned inside one L3 domain. If a floor comes back bimodal, check the core
  list first: a list straddling two domains lands client and server on different
  dies and splits the floors into two clusters about 2x apart, while every
  library row stays inside 1.4x.
- znet TCP cannot carry the 8KB case: `ZNET_MAX_BUFFER_SIZE` framing requires a
  whole message to fit in one buffer. The row reports `unsupported` rather than
  silently skipping.
