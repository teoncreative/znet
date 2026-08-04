# znet benchmarks

Benchmarks for znet's two transports, a raw-socket floor and three comparison
libraries, on clean loopback and under netem impairment. Every library runs the
same workloads through the same reporting code (`common/harness.h`), so rows
from different binaries line up into one table.

`fanout-bench` sits apart from that: one thread broadcasting 1 KiB to 8, 32 and
64 sessions, which is the shape a game server has rather than the one-session
pipeline everything else measures. It compares znet against itself, not against
the other libraries, and it does not participate in impaired runs.

**The comparison tables live in the [root README](../README.md#benchmarks)**, so
they sit next to the claims they support. This file is how to run them and how
to read them, plus the supporting measurements that are not part of that
comparison: the compression ratios per traffic type, what GNS's internal rate
clamp does to its rows, and the known gaps.

## Running

Benchmarks are off by default:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZNET_BUILD_BENCHMARKS=ON
cmake --build build --target benchmarks

# the front door: namespace + core pinning + every built binary in one command
./benchmarks/run.sh -c 0-7,16-23 build/benchmarks

# or by hand, in a namespace of its own
unshare -rn sh -c 'ip link set lo up
  taskset -c 0-7,16-23 ./build/benchmarks/znet-bench'
```

Knobs, honored by every binary (run.sh has flags for each):

| env | effect |
| --- | --- |
| `ZNET_BENCH_REPS=N` | run each case N times; the printed row is the median rep, with the span appended. The 64B column especially is not a number from one run. |
| `ZNET_BENCH_CSV=path` | append one machine-readable row per rep, for local diffing and aggregation. Not for CI: runner VMs cannot produce stable numbers. |
| `ZNET_BENCH_PAYLOAD=kind` | `binary` (default), `snapshot` or `text`; reproduces the compression table below |
| `ZNET_BENCH_SKIP_CONGESTION=1` | skip the congestion pool (~20 s per transport per profile) |

`znet-bench` takes three more, for narrowing a run while profiling:
`ZNET_BENCH_TRANSPORT=zdt|tcp` and `ZNET_BENCH_CASE=64B|1KB|8KB` keep one
transport or one case, `ZNET_BENCH_SKIP_LATENCY=1` drops the ping-pong, and
`ZNET_BENCH_METRICS=1` appends the session's protocol counters after each row.

**The suite does not measure CPU cost**, and the throughput rows are message and
byte rates only. Comparing efficiency would need the libraries to be doing the
same work per message, which they are not: only `znet_bench` builds and parses
an application-level message, while the comparison benches hand a pointer to the
library on send and free the buffer on receive without ever materialising one.
Making that comparable is a TODO, not something the current rows support.

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
of an unprivileged network namespace. The runner takes the spec once and
applies it to both netem and the benchmarks, so the two cannot disagree:

```sh
./benchmarks/run.sh -i "delay=25,loss=5" -c 0-7,16-23 build/benchmarks
```

Or by hand:

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
for scaling the workloads and labeling the output. If setting it by hand, give
it the same numbers you gave netem or the counts will be wrong.

`fanout-bench` is the exception: it never reads the variable, so under `-i` its
traffic crosses the impaired link while its workload stays unscaled and its rows
stay unlabelled. Name the binaries you want explicitly, or read its impaired
output as uncomparable with the rest.

Impaired throughput runs get an untimed warmup (25 RTTs, capped at 4 s)
followed by a drain before the clock starts, so the scaled-down message counts
measure the open window rather than slow-start; the congestion pool measures
the ramp itself. A run that still hits the 60 s harness deadline prints
`TIMEOUT` on its row instead of passing off the truncated rate as a result.
Both apply only to the shared loop in `harness.h`, so a delay-free (loss-only)
impairment gets no warmup, and `baseline-bench` gets neither: its raw-socket
loops are hand-written and run to completion however long that takes, which is
why a floor row can report a slower time than the 60 s cutoff above it.

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

The raw UDP row is a floor in the other direction. It is unreliable, so under
loss it simply drops what it loses and its count is what arrived, not what a
reliable protocol had to recover; read it as a datagram-rate ceiling rather than
as a competitor.

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

## The congestion pool

The workloads above ask how many messages per second a library manages, and on
clean loopback every one of them answers with what its memcpy path costs. That
number says nothing about the controller: a protocol with a fixed 64 KiB window
and one that grows to fill the link are indistinguishable when the round trip is
30 microseconds, because neither window is ever the binding constraint.

`common/congestion.h` is a second pool for that. Each case runs a bulk transfer
for a fixed duration while a small probe travels through it, and reports three
rows the throughput pool cannot:

| field        | what it says                                                                                            |
|--------------|---------------------------------------------------------------------------------------------------------|
| `ramp`       | when the sender first reached 90% of its own peak, i.e. how long the controller took to open the window |
| `steady`     | the rate over the second half of the run, so slow-start is not averaged into the number being compared  |
| `loaded-lat` | round trips for a 64 B probe sent *while* the transfer saturates the link                               |

`loaded-lat` is the one a fixed window and a delay-sensitive controller disagree
about most. Filling a deep queue and keeping it full costs nothing on the
throughput row and everything here. Read it against the ordinary `latency` row
for the same payload size: the difference is the standing queue the transfer
built.

| case | bulk  | probe | duration |
|------|-------|-------|----------|
| 1KB  | 1 KiB | 64 B  | 10 s     |
| 8KB  | 8 KiB | 64 B  | 10 s     |

Duration-based rather than message-count-based, so a case runs for the same wall
clock impaired and clean and needs none of the scaling the throughput pool gets.
`ZNET_BENCH_SKIP_CONGESTION=1` leaves the pool out of a znet run.

**These rows mean the most under impairment.** On a clean link nobody's window
binds and every ramp completes inside the first bucket, so `ramp` and `steady`
say very little; `loaded-lat` still does, and is where the clean numbers are
least flattering. Run them under netem for the other two to mean anything.

**Check the `sep` column before comparing two `loaded-lat` rows.** The probe
shares the connection with the transfer, on a separate channel where the
transport has channels (`sep channel`: ZDT, ENet, RakNet) and on the same stream
where it does not (`sep none`: znet over TCP, GNS). Without channels the probe
is head-of-line blocked behind the transfer as well as queued behind it, which
is a property of the transport's delivery model rather than of its controller.
A `sep none` row against a `sep channel` row is not a controller comparison.

The baseline runs the pool over kernel TCP with the probe on a second
connection (`sep conn`): both share the netem qdisc, so the probe reads the
queue the transfer built in the link. That row is cubic, a mature delay-blind
controller, and is the most direct reference for what ZDT's controller does
differently under the same impairment.

### What it showed

Under loss ZDT carries several times what GNS and ENet do while holding a small
message to a fraction of ENet's delay, which is the trade a delay-sensitive
controller exists to make. On a clean link the same rows are less flattering,
and that gap is an open item below. The numbers are in
[Under sustained load](../README.md#under-sustained-load).

## Reading the numbers

These are not apples-to-apples, and the differences matter more than the
ranking. Before comparing any two rows, check which of these applies.

**znet encrypts and compresses every packet by default**, via the existing
`EncryptionLayer` plus zstd. ENet and RakNet here send plaintext and do not
compress; only GNS is comparable on that axis, encrypting with AES-GCM as znet
does, so it is the only row paying a similar per-message crypto cost. So
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

**Socket buffers are lifted out of the way for every library.** The bench asks
16 MB everywhere it can: znet via `ZDTOptions::socket_recv_buffer`/`_send_buffer`
(library default 4 MB), ENet and RakNet by overriding their socket defaults
(256 KB for ENet; 256 KB receive and 16 KB send for RakNet) after startup.
`gns_bench` raises GNS's application-level limits; its socket buffer is a
hardcoded 256 KB with no config API. The kernel silently clamps every ask to
`net.core.rmem_max`/`wmem_max`. Raise those via sysctl for the asks to matter,
and check znet's debug log for what was actually granted. Protocol behavior
stays untouched: ENet's 64 KB
reliable window and every library's congestion controller are properties the
impaired runs exist to show, so no benchmark reconfigures them.

**Message rate is not byte rate.** A library that packs several small messages
into one datagram can report a message rate far above the raw UDP datagram
rate. That is real work, but it is measuring aggregation, not the socket. Check
the MiB/s column and the raw UDP floor together before concluding anything from
a msg/s number.

**GNS is rate-limited by its own design, and its rows are a lower bound.** Its
byte rate barely moves with payload size, 89, 96 and 90 MiB/s at 64 B, 1 KiB and
8 KiB, which is what a send-rate cap looks like rather than what protocol cost
looks like. znet's rows over the same three sizes go 102, 298 and 473 MiB/s,
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

Its latency tail is its own, though, not an artifact of the setup. `gns_bench`
reports two profiles rather than picking one. `gns` sets `NagleTime=0` so GNS
sends immediately as ZDT does; `gns-nagle` keeps the 5 ms coalescing GNS ships
with, which reports a ~10 ms round trip that measures Nagle rather than the
protocol.

Both are run rather than one being assumed harmless. The worry was that turning
Nagle off also disables the coalescing that should pay for itself at 64 B, the
one case GNS's internal send-rate clamp does not already pin, and therefore the
one case where the setting could change the throughput ranking.

The measurement says it does not. On the clean table `gns` beats `gns-nagle` at
64 B, 1,459,000 against 1,365,000, and matches it at 1 KiB and 8 KiB where the
clamp binds anyway, while costing 10 µs of round trip instead of 10.2 ms.
Under loss the two are inside each other's spread at every size. So Nagle off is
the like-for-like setting against znet on latency *and* is not paid for
elsewhere; the pair stays in the suite because that is a measured result rather
than an assumption. `GNS_NAGLE_US` overrides the `gns-nagle` profile's value.

It is also not doing the same work. GNS estimates bandwidth and paces its
sends where ZDT's controller only reads queueing delay, and GNS carries a
reliable byte stream rather than discrete messages, with NAT traversal, relay
fallback and certificate auth that nothing here exercises.

## Known gaps

- Under impairment RakNet misses the 60 s deadline at 1 KiB and 8 KiB. It does
  so steadily rather than intermittently, and the row says `TIMEOUT` rather than
  reading as a rate, so it is a property of RakNet on a lossy link rather than a
  measurement problem.
- **znet ZDT collapses intermittently under loss.** This is the largest caveat
  in the table. At 5% loss and a 50 ms round trip, single measurements drop to a
  fraction of the usual rate: across five runs the 8 KiB row spans 351..1,497
  msg/s (4.3x), 1 KiB spans 2,292..7,048 (3.1x) and even 64 B spans 3.2x, while
  the same cells on the clean table hold inside 1.14x. Counting the `-raw` arm
  too, every ZDT cell lands between 2.8x and 4.3x.
  It is per-measurement, not per-run: a run is routinely fast at one payload
  and collapsed at the next, which rules out the machine being busy for the
  duration. It is not the impairment being uneven either, since a collapsed and
  a healthy measurement sit in the same run under the same qdisc. GNS is not
  steady here either, spanning 2.7x at 1 KiB, though ENet is tighter than both
  at 1.5x on its worst cell. Unexplained; treat the impaired medians of every
  library as an average over regimes rather than a steady rate.
- The 64 B column needs several runs to mean anything. Scheduler noise dominates
  when a message costs this little. On the clean table the znet ZDT row spans
  1.02x across five runs, but under impairment the same column spans 3.2x for
  znet and 1.5x for ENet. Run it with `ZNET_BENCH_REPS=5` or more; the row is
  then the median with the span printed beside it.
- The raw TCP and raw UDP floors span 1.02-1.58x across five clean runs, the
  1.58x being raw TCP at 1 KiB, the noisiest cell on the clean table. A bimodal
  floor means the core list straddles two L3 domains; see Running above.
- **RakNet delivers nothing in the back half of the 8 KiB congestion case.** It
  bursts to a peak of ~134,000 msg/s, then its steady rate (the second half of
  the run) is exactly zero, reproducibly, in all five runs of two separate
  sweeps. The clean 8 KiB *throughput* row is healthy, so this is specific to a
  sustained transfer rather than the old 4999-of-5000 stall, which has not
  recurred since every case got its own port.
- znet TCP cannot carry the 8KB case: `ZNET_MAX_BUFFER_SIZE` framing requires a
  whole message to fit in one buffer. The row reports `unsupported` rather than
  silently skipping.
