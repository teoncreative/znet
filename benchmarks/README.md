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

Throughput rows also print `cpu-us/msg`, process-wide CPU time per delivered
message. znet spreads work over `hardware_concurrency()` threads while ENet is
single-threaded, so msg/s alone hides efficiency; this column is that axis.

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
for scaling the workloads and labelling the output. If setting it by hand, give
it the same numbers you gave netem or the counts will be wrong.

Impaired throughput runs get an untimed warmup (25 RTTs, capped at 4 s)
followed by a drain before the clock starts, so the scaled-down message counts
measure the open window rather than slow-start; the congestion pool measures
the ramp itself. A run that still hits the 60 s harness deadline prints
`TIMEOUT` on its row instead of passing off the truncated rate as a result.

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

## The congestion pool

The workloads above ask how many messages per second a library manages, and on
clean loopback every one of them answers with what its memcpy path costs. That
number says nothing about the controller: a protocol with a fixed 64 KiB window
and one that grows to fill the link are indistinguishable when the round trip is
30 microseconds, because neither window is ever the binding constraint.

`common/congestion.h` is a second pool for that. Each case runs a bulk transfer
for a fixed duration while a small probe travels through it, and reports three
rows the throughput pool cannot:

| field | what it says |
| --- | --- |
| `ramp` | when the sender first reached 90% of its own peak, i.e. how long the controller took to open the window |
| `steady` | the rate over the second half of the run, so slow-start is not averaged into the number being compared |
| `loaded-lat` | round trips for a 64 B probe sent *while* the transfer saturates the link |

`loaded-lat` is the one a fixed window and a delay-sensitive controller disagree
about most. Filling a deep queue and keeping it full costs nothing on the
throughput row and everything here. Read it against the ordinary `latency` row
for the same payload size: the difference is the standing queue the transfer
built.

| case | bulk | probe | duration |
| --- | --- | --- | --- |
| 1KB | 1 KiB | 64 B | 10 s |
| 8KB | 8 KiB | 64 B | 10 s |

Duration-based rather than message-count-based, so a case runs for the same wall
clock impaired and clean and needs none of the scaling the throughput pool gets.
`ZNET_BENCH_SKIP_CONGESTION=1` leaves the pool out of a znet run.

**These rows mean the most under impairment.** At a microsecond round trip
nobody's window binds, every ramp completes inside the first bucket, and the
loaded latency is the idle latency. Run them under netem or they say very
little.

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

**Payloads are incompressible by default** (`bench::MakePayload`): one repeated
byte flatters anything that compresses, since zstd takes 64 bytes of 'x' down to
about 15. `ZNET_BENCH_PAYLOAD=snapshot|text` switches every binary to the other
kinds, which is how this table was produced:

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

**Socket buffers are lifted out of the way for every library.** The bench asks
16 MB everywhere it can: znet via `ZDTOptions::socket_recv_buffer`/`_send_buffer`
(library default 4 MB), ENet and RakNet by overriding their socket defaults
(256 KB for ENet; 256 KB receive and 16 KB send for RakNet) after startup.
`gns_bench` raises GNS's application-level limits; its socket buffer is a
hardcoded 256 KB with no config API. The kernel silently clamps every ask to
`net.core.rmem_max`/`wmem_max` — raise those via sysctl for the asks to matter,
and check znet's debug log for what was actually granted. Whoever shipped the smallest buffer is not what
the table is meant to measure. Protocol behavior stays untouched: ENet's 64 KB
reliable window and every library's congestion controller are properties the
impaired runs exist to show, so no benchmark reconfigures them.

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

Its latency tail is its own, though, not an artifact of the setup. `gns_bench`
reports two profiles rather than picking one. `gns` sets `NagleTime=0` so GNS
sends immediately as ZDT does; `gns-nagle` keeps the 5 ms coalescing GNS ships
with, which reports a ~10 ms round trip that measures Nagle rather than the
protocol.

Both are run because turning Nagle off is not free in one direction only. It is
the like-for-like setting against znet on latency, but it also disables the
coalescing that pays for itself at 64 B — which is the one case GNS's internal
send-rate clamp does not already pin, and therefore the one case where the
setting could plausibly change the throughput ranking. Running the pair says
what it is worth per payload size instead of assuming it is worth nothing.
`GNS_NAGLE_US` overrides the `gns-nagle` profile's value.

It is also not doing the same work. GNS estimates bandwidth and paces its
sends where ZDT's controller only reads queueing delay, and GNS carries a
reliable byte stream rather than discrete messages, with NAT traversal, relay
fallback and certificate auth that nothing here exercises.

## Known gaps

- **RakNet's published 8 KiB throughput row is stale and should be re-measured.**
  It intermittently stalled at 4999 of 5000 delivered and ran into the 60 s
  deadline, reporting ~83 msg/s against ~74,000 for a successful run — one run
  in three for the published table. That was measured while all three throughput
  cases shared port 47200: `Shutdown(100)` returns before a peer's last
  datagrams have drained, and the next case binding the same port inherits them.
  `gns_bench` already gave every case its own port for exactly this reason;
  `enet_bench` and `raknet_bench` now do too, and RakNet's `Send` return value
  is no longer discarded, so a refusal shows up instead of quietly advancing the
  backlog accounting. A run that still stalls now prints `TIMEOUT` on the row
  rather than a plausible-looking rate. Whether the stall survives is unmeasured.
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
  4.6x for znet and 5.4x for ENet. Run it with `ZNET_BENCH_REPS=5` or more; the
  row is then the median with the span printed beside it.
- The raw TCP and raw UDP floors span 1.03-1.19x across seven runs with the core
  list pinned inside one L3 domain. If a floor comes back bimodal, check the core
  list first: a list straddling two domains lands client and server on different
  dies and splits the floors into two clusters about 2x apart, while every
  library row stays inside 1.4x.
- znet TCP cannot carry the 8KB case: `ZNET_MAX_BUFFER_SIZE` framing requires a
  whole message to fit in one buffer. The row reports `unsupported` rather than
  silently skipping.
